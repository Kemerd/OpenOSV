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

unsigned char* HostImage::rawPixel(int x, int y) noexcept {
    if (x < bounds.x1 || x >= bounds.x2 || y < bounds.y1 || y >= bounds.y2) {
        return nullptr;
    }
    unsigned char* row =
        reinterpret_cast<unsigned char*>(data()) + static_cast<std::ptrdiff_t>(y - bounds.y1) * rowBytes;
    return row + static_cast<std::ptrdiff_t>(x - bounds.x1) * pixelBytes();
}

const unsigned char* HostImage::rawPixel(int x, int y) const noexcept {
    return const_cast<HostImage*>(this)->rawPixel(x, y);
}

float* HostImage::pixel(int x, int y) noexcept {
    // Float images only: an 8-bit pixel has no floats to point at.
    if (depth != osv::ofx::HostDepth::Float) {
        return nullptr;
    }
    return reinterpret_cast<float*>(rawPixel(x, y));
}

const float* HostImage::pixel(int x, int y) const noexcept {
    return const_cast<HostImage*>(this)->pixel(x, y);
}

bool HostImage::readRgba(int x, int y, float rgba[4]) const noexcept {
    const unsigned char* p = rawPixel(x, y);
    if (!p || !rgba) {
        return false;
    }
    osv::ofx::loadHostPixel(p, depth, order, rgba);
    return true;
}

bool HostImage::writeRgba(int x, int y, const float rgba[4]) noexcept {
    unsigned char* p = rawPixel(x, y);
    if (!p || !rgba) {
        return false;
    }
    osv::ofx::storeHostPixel(p, depth, order, osv::ofx::OutputLevels::Full, rgba);
    return true;
}

void HostImage::describe(PropertySet& image) noexcept {
    image.setPointer(kOfxImagePropData, data());
    image.setInts(kOfxImagePropBounds, {bounds.x1, bounds.y1, bounds.x2, bounds.y2});
    image.setInts(kOfxImagePropRegionOfDefinition, {bounds.x1, bounds.y1, bounds.x2, bounds.y2});
    image.setInt(kOfxImagePropRowBytes, rowBytes);
    // The depth's own label unless a test asked for another one.
    const char* ownDepth = depth == osv::ofx::HostDepth::Byte ? kOfxBitDepthByte : kOfxBitDepthFloat;
    image.setString(kOfxImageEffectPropPixelDepth, depthLabel.empty() ? std::string(ownDepth) : depthLabel);
    image.setString(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
    image.setString(kOfxImageEffectPropPreMultiplication, kOfxImageUnPreMultiplied);
    image.setDouble(kOfxImagePropPixelAspectRatio, 1.0);
    image.setString(kOfxImagePropField, kOfxImageFieldNone);
    image.setDoubles(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    if (labelOrder) {
        image.setString(osv::ofx::kPropPixelOrder,
                        order == osv::ofx::HostOrder::Bgra ? osv::ofx::kPixelOrderBgra : osv::ofx::kPixelOrderRgba);
    }
}

void HostImage::fill(float value) noexcept {
    if (depth == osv::ofx::HostDepth::Byte) {
        fillBytes(kByteSentinel);
        return;
    }
    std::fill(storage.begin(), storage.end(), value);
}

void HostImage::fillBytes(std::uint8_t code) noexcept {
    if (!storage.empty()) {
        std::memset(storage.data(), code, storage.size() * sizeof(float));
    }
}

HostImage makeImage(const OfxRectI& bounds, bool negativePitch, int padFloats, osv::ofx::HostDepth depth,
                    osv::ofx::HostOrder order) {
    HostImage img;
    img.bounds = bounds;
    img.depth = depth;
    img.order = order;
    const int w = std::max(0, bounds.x2 - bounds.x1);
    const int h = std::max(0, bounds.y2 - bounds.y1);
    // One float of storage holds one 8-bit pixel, four hold a float pixel.
    const int floatsPerPixel = depth == osv::ofx::HostDepth::Byte ? 1 : 4;
    const int rowFloats = w * floatsPerPixel + std::max(0, padFloats);
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

HostImage convertImage(const HostImage& source, osv::ofx::HostDepth depth, osv::ofx::HostOrder order,
                       bool negativePitch, int padFloats) {
    HostImage out = makeImage(source.bounds, negativePitch, padFloats, depth, order);
    for (int y = source.bounds.y1; y < source.bounds.y2; ++y) {
        for (int x = source.bounds.x1; x < source.bounds.x2; ++x) {
            float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (source.readRgba(x, y, rgba)) {
                out.writeRgba(x, y, rgba);
            }
        }
    }
    return out;
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
            // Three independent smooth fields: no mirror or rotation of the
            // sphere maps the picture onto itself.
            const float p[4] = {
                static_cast<float>(0.5 + 0.45 * std::sin(lon + 0.3) * std::cos(lat)),
                static_cast<float>(0.5 + 0.45 * std::sin(lat * 1.3 + 0.2)),
                static_cast<float>(0.5 + 0.35 * std::cos(2.0 * lon - 0.7) * std::cos(lat) + 0.1 * std::sin(lat)),
                1.0f,
            };
            // Any format: a float image takes the values exactly as they are.
            image.writeRgba(x, y, p);
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
            // Any source format, read as the straight RGBA floats the
            // plug-in's sampler sees (8-bit codes promoted by 1/255).
            float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            (void)source.readRgba(source.bounds.x1 + x, y, p);
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
            // RGBA floats whatever either image's format (a float RGBA
            // image reads back exactly the floats it holds).
            float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float q[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (!a.readRgba(x, y, p) || !b.readRgba(x, y, q)) {
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

double maxPackedDifference(const HostImage& image, const HostImage& reference, const OfxRectI& window,
                           osv::ofx::OutputLevels levels) {
    double worst = 0.0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const unsigned char* got = image.rawPixel(x, y);
            float ref[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (!got || !reference.readRgba(x, y, ref)) {
                return std::numeric_limits<double>::infinity();
            }
            // What the packing rule makes of the reference in this format.
            alignas(16) unsigned char expected[16] = {};
            osv::ofx::storeHostPixel(expected, image.depth, image.order, levels, ref);
            for (int c = 0; c < 4; ++c) {
                double d = 0.0;
                if (image.depth == osv::ofx::HostDepth::Byte) {
                    d = std::fabs(static_cast<double>(got[c]) - static_cast<double>(expected[c]));
                } else {
                    float a = 0.0f;
                    float b = 0.0f;
                    std::memcpy(&a, got + c * 4, sizeof(float));
                    std::memcpy(&b, expected + c * 4, sizeof(float));
                    d = std::fabs(static_cast<double>(a) - static_cast<double>(b));
                }
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
