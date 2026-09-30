// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_ofx_gpu_pipeline.cpp - the own-GPU path's pipeline (plugins/ofx/
// OfxGpuPipeline.h), driven directly with every pixel format, window, pitch
// and level a CPU-image host (VEGAS Pro) can hand the effects.
//
// ===========================================================================
//  What "correct" means here
// ===========================================================================
// The reference is what the CPU paths write, built from the SAME pieces they
// use: an 8-bit source promoted to float exactly as
// reframe::promoteIntegerToFloat() does (code * (1/255)), the one per-pixel
// function osvReframeEquirectPixel(), then OfxHostImage.h's storeHostPixel()
// - the one packing rule (levels, depth, order), transparent black outside
// the camera frame included.  So:
//
//   * 8-bit output within +-1 code of the reference (the GPU's atan2f /
//     asinf differ from the CPU's in the last ulp, which can move a sample
//     across a rounding boundary);
//   * float output at or above 60 dB PSNR, the bar the existing CUDA-image
//     filter path is held to (test_ofx_cuda.cpp);
//   * the 360 equirect pack - a copy, no sampling - EXACT;
//   * not one byte outside the render window (or in a row's padding) touched.
//
// Every [cuda] test needs an NVIDIA device and SKIPs without one.  The tests
// without [cuda] check refusals that happen before any GPU work, and run
// anywhere.

#include "OfxGpuPipeline.h"
#include "OfxHostImage.h"

#include "ReframeCpu.h"

#include "osv/core/ThreadPool.h"
#include "osv/render/osv_kernel.h"

#include <catch2/catch_test_macros.hpp>

#include <cuda.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace osv::ofx;
using namespace osv::ofx::gpu;
namespace rf = osv::reframe;

namespace {

constexpr double kPi = 3.14159265358979323846;

/// What an untouched byte holds: every image is filled with it first, so a
/// write outside the render window (or into a row's padding) shows.
constexpr unsigned char kSentinel = 0xA5;

// ===========================================================================
//  Images in every layout a host may hand out
// ===========================================================================

/// An OpenFX image of either depth and channel order: pixel (x, y), y UP,
/// at data + (y - bounds.y1) * rowBytes + (x - bounds.x1) * bpp.  The pitch
/// may carry padding and may be negative (rows stored top-down).
struct TestImage {
    OfxRectI bounds{0, 0, 0, 0};
    HostDepth depth = HostDepth::Float;
    HostOrder order = HostOrder::Rgba;
    int rowBytes = 0;                    ///< Signed pitch.
    std::size_t stride = 0;              ///< |rowBytes|.
    std::vector<unsigned char> storage;  ///< Every row, padding included.
    std::size_t bottomOffset = 0;        ///< Byte offset of the bottom row in `storage`.

    [[nodiscard]] int width() const noexcept { return bounds.x2 - bounds.x1; }
    [[nodiscard]] int height() const noexcept { return bounds.y2 - bounds.y1; }
    [[nodiscard]] int bpp() const noexcept { return bytesPerPixel(depth); }

    [[nodiscard]] HostImageView view() noexcept {
        return HostImageView{storage.data() + bottomOffset, rowBytes, bounds, depth, order};
    }
    [[nodiscard]] unsigned char* row(int y) noexcept {
        return storage.data() + bottomOffset + static_cast<std::ptrdiff_t>(y - bounds.y1) * rowBytes;
    }
    [[nodiscard]] const unsigned char* row(int y) const noexcept {
        return storage.data() + bottomOffset + static_cast<std::ptrdiff_t>(y - bounds.y1) * rowBytes;
    }
    [[nodiscard]] unsigned char* pixel(int x, int y) noexcept {
        return row(y) + static_cast<std::ptrdiff_t>(x - bounds.x1) * bpp();
    }
    [[nodiscard]] const unsigned char* pixel(int x, int y) const noexcept {
        return row(y) + static_cast<std::ptrdiff_t>(x - bounds.x1) * bpp();
    }
};

/// A sentinel-filled image; `padBytes` of padding per row (a multiple of 4
/// keeps float rows aligned, as real hosts do).
TestImage makeImage(const OfxRectI& bounds, HostDepth depth, HostOrder order, bool negativePitch = false,
                    int padBytes = 0) {
    TestImage img;
    img.bounds = bounds;
    img.depth = depth;
    img.order = order;
    const int w = std::max(0, bounds.x2 - bounds.x1);
    const int h = std::max(0, bounds.y2 - bounds.y1);
    img.stride = static_cast<std::size_t>(w) * static_cast<std::size_t>(bytesPerPixel(depth)) +
                 static_cast<std::size_t>(std::max(0, padBytes));
    img.storage.assign(img.stride * static_cast<std::size_t>(h), kSentinel);
    if (negativePitch) {
        // Top row first in memory: the bottom row is the LAST one.
        img.rowBytes = -static_cast<int>(img.stride);
        img.bottomOffset = img.stride * static_cast<std::size_t>(h > 0 ? h - 1 : 0);
    } else {
        img.rowBytes = static_cast<int>(img.stride);
        img.bottomOffset = 0;
    }
    return img;
}

/// Store straight R,G,B,A in the image's depth and order at full levels (a
/// source image as a host would hold it).
void storePlain(unsigned char* p, HostDepth depth, HostOrder order, const float rgba[4]) {
    storeHostPixel(p, depth, order, OutputLevels::Full, rgba);
}

/// Store as the CPU paths store a rendered pixel: OfxHostImage.h's one
/// packing rule (levels on R, G, B, never alpha; then the depth; then the
/// order).
void storeLevelled(unsigned char* p, HostDepth depth, HostOrder order, const float rgba[4], OutputLevels levels) {
    storeHostPixel(p, depth, order, levels, rgba);
}

/// Transparent black as the CPU paths write it outside the camera frame:
/// (0, 0, 0, 0) BEFORE levels, so RGB at studio black under studio levels.
void storeClear(unsigned char* p, HostDepth depth, HostOrder order, OutputLevels levels) {
    const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    storeHostPixel(p, depth, order, levels, transparent);
}

/// The test panorama (the same three independent smooth fields as
/// tests/ofx's paintPanorama, plus an alpha that varies, so alpha is checked
/// too), row 0 of the equirect at the TOP.
void panoramaAt(int rowFromTop, int column, int w, int h, float rgba[4]) {
    const double lat = kPi / 2.0 - (rowFromTop + 0.5) / h * kPi;
    const double lon = (column + 0.5) / w * 2.0 * kPi - kPi;
    rgba[0] = static_cast<float>(0.5 + 0.45 * std::sin(lon + 0.3) * std::cos(lat));
    rgba[1] = static_cast<float>(0.5 + 0.45 * std::sin(lat * 1.3 + 0.2));
    rgba[2] = static_cast<float>(0.5 + 0.35 * std::cos(2.0 * lon - 0.7) * std::cos(lat) + 0.1 * std::sin(lat));
    rgba[3] = static_cast<float>(0.8 + 0.2 * std::cos(lon * 3.0));
}

/// Paint the panorama into a host image (y up: its top row is row 0).
void paintPanorama(TestImage& img) {
    for (int y = img.bounds.y1; y < img.bounds.y2; ++y) {
        const int rowFromTop = img.bounds.y2 - 1 - y;
        for (int x = img.bounds.x1; x < img.bounds.x2; ++x) {
            float rgba[4];
            panoramaAt(rowFromTop, x - img.bounds.x1, img.width(), img.height(), rgba);
            storePlain(img.pixel(x, y), img.depth, img.order, rgba);
        }
    }
}

// ===========================================================================
//  The reference: the CPU paths' own pieces
// ===========================================================================

/// A host source as the CPU samples it: promoted to float exactly as
/// reframe::promoteIntegerToFloat() does, rows bottom-up (the OpenFX order,
/// as buildParams() describes an OpenFX image), channel order kept.
struct Promoted {
    std::vector<float> data;
    OsvRgbaSource source{};
    const void* row0 = nullptr;
};

Promoted promote(const TestImage& img) {
    Promoted p;
    const int w = img.width();
    const int h = img.height();
    p.data.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    const float scale = 1.0f / 255.0f;  // promoteIntegerToFloat()'s 8-bit scale
    for (int i = 0; i < h; ++i) {
        const unsigned char* in = img.row(img.bounds.y1 + i);
        float* out = p.data.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(w) * 4u;
        for (int c = 0; c < w * 4; ++c) {
            if (img.depth == HostDepth::Byte) {
                out[c] = static_cast<float>(in[c]) * scale;
            } else {
                std::memcpy(&out[c], in + static_cast<std::size_t>(c) * 4u, sizeof(float));
            }
        }
    }
    p.source.w = w;
    p.source.h = h;
    p.source.pitchBytes = w * 16;
    p.source.isHalf = 0;
    p.source.isBgra = img.order == HostOrder::Bgra ? 1 : 0;
    p.source.flipY = 1;  // the pointer is the BOTTOM row
    p.row0 = p.data.data();
    return p;
}

/// The intersection of two rectangles (empty when disjoint).
OfxRectI intersectRect(const OfxRectI& a, const OfxRectI& b) {
    OfxRectI r{std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2), std::min(a.y2, b.y2)};
    if (r.x2 < r.x1) {
        r.x2 = r.x1;
    }
    if (r.y2 < r.y1) {
        r.y2 = r.y1;
    }
    return r;
}

[[nodiscard]] bool inside(const OfxRectI& r, int x, int y) { return x >= r.x1 && x < r.x2 && y >= r.y1 && y < r.y2; }

/// The CPU view: every pixel of window x bounds, framed from `src` through
/// osvReframeEquirectPixel(); transparent black outside the camera frame.
void referenceView(const OsvReframeParams& params, const OsvRgbaSource& src, const void* row0,
                   const HostTarget& target, TestImage& out) {
    const OfxRectI area = intersectRect(target.window, out.bounds);
    for (int y = area.y1; y < area.y2; ++y) {
        for (int x = area.x1; x < area.x2; ++x) {
            unsigned char* p = out.pixel(x, y);
            if (!inside(target.frame, x, y)) {
                storeClear(p, out.depth, out.order, target.levels);
                continue;
            }
            float rgba[4];
            osvReframeEquirectPixel(&params, &src, row0, x - target.frame.x1, target.frame.y2 - 1 - y, rgba);
            storeLevelled(p, out.depth, out.order, rgba, target.levels);
        }
    }
}

/// The CPU equirect: the top-down float RGBA `image` IS the camera frame.
void referenceEquirect(const std::vector<float>& image, int w, int h, const HostTarget& target, TestImage& out) {
    const OfxRectI area = intersectRect(target.window, out.bounds);
    for (int y = area.y1; y < area.y2; ++y) {
        for (int x = area.x1; x < area.x2; ++x) {
            unsigned char* p = out.pixel(x, y);
            const int col = x - target.frame.x1;
            const int rowFromTop = target.frame.y2 - 1 - y;
            if (!inside(target.frame, x, y) || col >= w || rowFromTop >= h) {
                storeClear(p, out.depth, out.order, target.levels);
                continue;
            }
            const float* texel = image.data() + (static_cast<std::size_t>(rowFromTop) * w + col) * 4u;
            storeLevelled(p, out.depth, out.order, texel, target.levels);
        }
    }
}

/// How far a GPU image is from its reference.
struct Diff {
    int maxCode = 0;          ///< Largest 8-bit code difference inside the window.
    double maxFloat = 0.0;    ///< Largest float difference inside the window.
    double psnr = std::numeric_limits<double>::infinity();  ///< Float PSNR over the window (peak 1).
    bool outsideTouched = false;  ///< A byte outside the window (or padding) differs.
    bool nonFinite = false;       ///< A float inside the window is NaN / inf on one side only.
};

Diff compare(const TestImage& gpu, const TestImage& ref, const OfxRectI& window) {
    Diff d;
    const OfxRectI area = intersectRect(window, gpu.bounds);
    double sum = 0.0;
    std::size_t n = 0;
    const int bpp = gpu.bpp();
    for (int y = gpu.bounds.y1; y < gpu.bounds.y2; ++y) {
        const unsigned char* a = gpu.row(y);
        const unsigned char* b = ref.row(y);
        // Every byte of the row, padding included.
        for (std::size_t i = 0; i < gpu.stride; ++i) {
            const int x = gpu.bounds.x1 + static_cast<int>(i / static_cast<std::size_t>(bpp));
            const bool inPixel = i < static_cast<std::size_t>(gpu.width()) * static_cast<std::size_t>(bpp);
            if (!inPixel || !inside(area, x, y)) {
                if (a[i] != b[i]) {
                    d.outsideTouched = true;
                }
                continue;
            }
            if (gpu.depth == HostDepth::Byte) {
                d.maxCode = std::max(d.maxCode, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
            } else if (i % 4u == 0) {
                float fa = 0.0f;
                float fb = 0.0f;
                std::memcpy(&fa, a + i, sizeof(float));
                std::memcpy(&fb, b + i, sizeof(float));
                if (std::isfinite(fa) != std::isfinite(fb)) {
                    d.nonFinite = true;
                    continue;
                }
                if (!std::isfinite(fa)) {
                    continue;
                }
                const double diff = static_cast<double>(fa) - static_cast<double>(fb);
                d.maxFloat = std::max(d.maxFloat, std::fabs(diff));
                sum += diff * diff;
                ++n;
            }
        }
    }
    if (n > 0 && sum > 0.0) {
        d.psnr = 10.0 * std::log10(1.0 / (sum / static_cast<double>(n)));
    }
    return d;
}

/// The camera: buildView() - what buildParams() gives the kernel - for a
/// `w` x `h` frame.  False when no camera could be built.  No Catch
/// assertion inside: the concurrency test calls it from worker threads.
bool cameraInto(double pan, double tilt, double fov, int w, int h, bool dji, OsvReframeParams& out) {
    rf::Settings s;
    s.resolution = rf::sanitiseResolution(OSV_REFRAME_RESOLUTION_DEFAULT);
    s.preset = rf::sanitisePreset(OSV_REFRAME_PRESET_DEFAULT);
    s.cameraModel = dji ? rf::CameraModel::Dji : rf::CameraModel::Classic;
    s.panDeg = pan;
    s.tiltDeg = tilt;
    if (dji) {
        s.djiFovDeg = fov;
    } else {
        s.fovDeg = fov;
    }
    const rf::ViewSetup view = rf::buildView(s, w, h, rf::SizePx{w, h});
    out = view.params;
    return view.valid;
}

/// cameraInto() for the test's own thread.
OsvReframeParams camera(double pan, double tilt, double fov, int w, int h, bool dji = true) {
    OsvReframeParams params{};
    REQUIRE(cameraInto(pan, tilt, fov, w, h, dji, params));
    return params;
}

const char* depthName(HostDepth d) { return d == HostDepth::Byte ? "byte" : "float"; }
const char* orderName(HostOrder o) { return o == HostOrder::Bgra ? "BGRA" : "RGBA"; }
const char* levelsName(OutputLevels l) { return l == OutputLevels::Studio ? "studio" : "full"; }

/// Hold the GPU result to the reference by the file's rules.
void checkClose(const TestImage& gpu, const TestImage& ref, const OfxRectI& window) {
    const Diff d = compare(gpu, ref, window);
    INFO("max code " << d.maxCode << ", max float " << d.maxFloat << ", PSNR " << d.psnr);
    CHECK_FALSE(d.outsideTouched);
    CHECK_FALSE(d.nonFinite);
    if (gpu.depth == HostDepth::Byte) {
        CHECK(d.maxCode <= 1);
    } else {
        CHECK(d.psnr >= 60.0);
    }
}

// ===========================================================================
//  CUDA
// ===========================================================================

/// True when an NVIDIA driver and a device are there; the reason otherwise.
bool cudaUsable(std::string& why) {
    // From System32 only; kept loaded so the delay-load stubs bind to it.
    if (!::LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        why = "no NVIDIA driver (nvcuda.dll)";
        return false;
    }
    if (cuInit(0) != CUDA_SUCCESS) {
        why = "cuInit failed";
        return false;
    }
    int count = 0;
    if (cuDeviceGetCount(&count) != CUDA_SUCCESS || count <= 0) {
        why = "no CUDA device";
        return false;
    }
    return true;
}

#define REQUIRE_CUDA()                  \
    do {                                \
        std::string whyNot_;            \
        if (!cudaUsable(whyNot_)) {     \
            SKIP(whyNot_);              \
        }                               \
    } while (false)

/// The context current on this thread (null when none).
CUcontext currentContext() {
    CUcontext c = nullptr;
    REQUIRE(cuCtxGetCurrent(&c) == CUDA_SUCCESS);
    return c;
}

/// Pop every context off this thread's stack.
void popAll() {
    CUcontext top = nullptr;
    while (cuCtxGetCurrent(&top) == CUDA_SUCCESS && top) {
        CUcontext popped = nullptr;
        REQUIRE(cuCtxPopCurrent(&popped) == CUDA_SUCCESS);
    }
}

/// A float R,G,B,A panorama in VRAM - the stand-in for the engine's stitched
/// sphere: device 0's PRIMARY context (the one the CUDA runtime renders in),
/// a padded pitch like cudaMallocPitch's, top row first.
struct DeviceSphere {
    CUdevice device = 0;
    CUcontext primary = nullptr;
    CUdeviceptr base = 0;
    std::size_t pitch = 0;
    int w = 0;
    int h = 0;
    std::vector<float> host;  ///< The same pixels, top-down, tight.

    DeviceSphere(int width, int height, bool inPrivateContext = false, CUcontext* privateOut = nullptr)
        : w(width), h(height) {
        REQUIRE(cuDeviceGet(&device, 0) == CUDA_SUCCESS);
        host.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
        for (int r = 0; r < h; ++r) {
            for (int c = 0; c < w; ++c) {
                panoramaAt(r, c, w, h, host.data() + (static_cast<std::size_t>(r) * w + c) * 4u);
            }
        }
        CUcontext context = nullptr;
        if (inPrivateContext) {
            REQUIRE(cuCtxCreate(&context, 0, device) == CUDA_SUCCESS);
            REQUIRE(privateOut);
            *privateOut = context;
            owned = context;
        } else {
            REQUIRE(cuDevicePrimaryCtxRetain(&primary, device) == CUDA_SUCCESS);
            REQUIRE(cuCtxPushCurrent(primary) == CUDA_SUCCESS);
        }
        REQUIRE(cuMemAllocPitch(&base, &pitch, static_cast<std::size_t>(w) * 16u, static_cast<std::size_t>(h), 16) ==
                CUDA_SUCCESS);
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_HOST;
        copy.srcHost = host.data();
        copy.srcPitch = static_cast<std::size_t>(w) * 16u;
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = base;
        copy.dstPitch = pitch;
        copy.WidthInBytes = static_cast<std::size_t>(w) * 16u;
        copy.Height = static_cast<std::size_t>(h);
        REQUIRE(cuMemcpy2D(&copy) == CUDA_SUCCESS);
        CUcontext popped = nullptr;
        REQUIRE(cuCtxPopCurrent(&popped) == CUDA_SUCCESS);
    }
    ~DeviceSphere() {
        CUcontext context = owned ? owned : primary;
        if (context && cuCtxPushCurrent(context) == CUDA_SUCCESS) {
            if (base) {
                cuMemFree(base);
            }
            CUcontext popped = nullptr;
            cuCtxPopCurrent(&popped);
        }
        if (owned) {
            cuCtxDestroy(owned);
        } else if (primary) {
            cuDevicePrimaryCtxRelease(device);
        }
    }
    DeviceSphere(const DeviceSphere&) = delete;
    DeviceSphere& operator=(const DeviceSphere&) = delete;

    [[nodiscard]] DeviceRgba rgba() const {
        DeviceRgba d;
        d.data = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(base));
        d.pitchBytes = pitch;
        d.width = static_cast<std::uint32_t>(w);
        d.height = static_cast<std::uint32_t>(h);
        d.device = 0;
        return d;
    }
    /// The CPU's view of the same pixels (top-down, R,G,B,A).
    [[nodiscard]] OsvRgbaSource source() const {
        OsvRgbaSource s{};
        s.w = w;
        s.h = h;
        s.pitchBytes = w * 16;
        s.isHalf = 0;
        s.isBgra = 0;
        s.flipY = 0;
        return s;
    }

private:
    CUcontext owned = nullptr;  ///< A private context this object created (then no primary retain).
};

/// The render windows every format is tried with: the whole frame, a
/// rectangle inside it, and one wider than the image (clipped by the path).
struct WindowCase {
    const char* name;
    OfxRectI window;
};

std::vector<WindowCase> windowsFor(const OfxRectI& bounds) {
    return {
        {"whole image", bounds},
        {"inner rectangle", OfxRectI{bounds.x1 + 37, bounds.y1 + 21, bounds.x2 - 53, bounds.y2 - 17}},
        {"wider than the image", OfxRectI{bounds.x1 - 9, bounds.y1 - 5, bounds.x2 + 11, bounds.y2 + 7}},
    };
}

/// The pitches every format is tried with.
struct LayoutCase {
    const char* name;
    bool negative;
    int padBytes;
};

constexpr LayoutCase kLayouts[] = {
    {"tight", false, 0},
    {"padded", false, 48},
    {"negative pitch", true, 0},
    {"negative padded pitch", true, 16},
};

}  // namespace

// ===========================================================================
//  Refusals before any GPU work (no device needed)
// ===========================================================================

TEST_CASE("an unusable target or source is refused and nothing is written", "[ofxgpu]") {
    const OsvReframeParams params{};  // never reached: the refusals come first
    TestImage source = makeImage(OfxRectI{0, 0, 64, 32}, HostDepth::Float, HostOrder::Rgba);
    TestImage output = makeImage(OfxRectI{0, 0, 32, 16}, HostDepth::Byte, HostOrder::Bgra);
    const std::vector<unsigned char> untouched = output.storage;

    HostTarget target;
    target.image = output.view();
    target.window = output.bounds;
    target.frame = output.bounds;

    SECTION("no target data") {
        target.image.data = nullptr;
        std::string error;
        CHECK_FALSE(frameHostImage(params, source.view(), target, 0, nullptr, error));
        CHECK_FALSE(error.empty());
    }
    SECTION("a target pitch shorter than a row") {
        target.image.rowBytes = 8;
        std::string error;
        CHECK_FALSE(frameHostImage(params, source.view(), target, 0, nullptr, error));
        CHECK_FALSE(error.empty());
    }
    SECTION("a source pitch shorter than a row") {
        HostImageView bad = source.view();
        bad.rowBytes = -16;
        std::string error;
        CHECK_FALSE(frameHostImage(params, bad, target, 0, nullptr, error));
        CHECK_FALSE(error.empty());
    }
    SECTION("an empty camera frame") {
        target.frame = OfxRectI{5, 5, 5, 9};
        std::string error;
        CHECK_FALSE(frameHostImage(params, source.view(), target, 0, nullptr, error));
        CHECK_FALSE(error.empty());
    }
    SECTION("a read-back with nothing framed") {
        FrameJob job;
        std::string error;
        CHECK_FALSE(job.readBack(nullptr, error));
        CHECK_FALSE(error.empty());
    }
    CHECK(output.storage == untouched);
}

TEST_CASE("a render window that misses the image succeeds and writes nothing", "[ofxgpu]") {
    const OsvReframeParams params{};
    TestImage source = makeImage(OfxRectI{0, 0, 64, 32}, HostDepth::Byte, HostOrder::Rgba);
    TestImage output = makeImage(OfxRectI{0, 0, 32, 16}, HostDepth::Float, HostOrder::Rgba);
    const std::vector<unsigned char> untouched = output.storage;
    HostTarget target;
    target.image = output.view();
    target.window = OfxRectI{100, 100, 140, 120};  // nowhere near the image
    target.frame = output.bounds;
    std::string error;
    CHECK(frameHostImage(params, source.view(), target, 0, nullptr, error));
    CHECK(error.empty());
    CHECK(output.storage == untouched);
}

// ===========================================================================
//  The filter on CPU images
// ===========================================================================

TEST_CASE("the GPU filter matches the CPU path for every depth, order, window and pitch", "[ofxgpu][cuda]") {
    REQUIRE_CUDA();
    osv::ThreadPool pool(4);
    const OfxRectI sourceBounds{0, 0, 512, 256};
    // The output sits away from the origin, as a host's render window can.
    const OfxRectI outBounds{12, 20, 12 + 320, 20 + 180};
    const OsvReframeParams params = camera(64.0, 18.0, 95.0, 320, 180);

    for (const HostDepth depth : {HostDepth::Byte, HostDepth::Float}) {
        for (const HostOrder order : {HostOrder::Rgba, HostOrder::Bgra}) {
            for (const LayoutCase& layout : kLayouts) {
                for (const WindowCase& wc : windowsFor(outBounds)) {
                    INFO(depthName(depth) << " " << orderName(order) << ", " << layout.name << ", " << wc.name);
                    TestImage source =
                        makeImage(sourceBounds, depth, order, !layout.negative, layout.padBytes ? 32 : 0);
                    paintPanorama(source);
                    TestImage gpu = makeImage(outBounds, depth, order, layout.negative, layout.padBytes);
                    TestImage ref = gpu;

                    HostTarget target;
                    target.image = gpu.view();
                    target.window = wc.window;
                    target.frame = outBounds;
                    target.levels = OutputLevels::Full;  // the filter keeps the host's levels
                    std::string error;
                    const bool ok = frameHostImage(params, source.view(), target, 0, &pool, error);
                    INFO(error);
                    REQUIRE(ok);

                    const Promoted promoted = promote(source);
                    referenceView(params, promoted.source, promoted.row0, target, ref);
                    checkClose(gpu, ref, wc.window);
                }
            }
        }
    }
}

TEST_CASE("the GPU filter converts between depths, orders and levels", "[ofxgpu][cuda]") {
    REQUIRE_CUDA();
    struct Mix {
        HostDepth srcDepth;
        HostOrder srcOrder;
        HostDepth dstDepth;
        HostOrder dstOrder;
        OutputLevels levels;
    };
    const Mix mixes[] = {
        {HostDepth::Byte, HostOrder::Bgra, HostDepth::Float, HostOrder::Rgba, OutputLevels::Full},
        {HostDepth::Float, HostOrder::Rgba, HostDepth::Byte, HostOrder::Bgra, OutputLevels::Studio},
        {HostDepth::Byte, HostOrder::Rgba, HostDepth::Byte, HostOrder::Bgra, OutputLevels::Studio},
        {HostDepth::Float, HostOrder::Bgra, HostDepth::Float, HostOrder::Bgra, OutputLevels::Studio},
    };
    const OfxRectI outBounds{0, 0, 256, 144};
    // The Classic lens too, and a rolled-over tilt.
    const OsvReframeParams params = camera(-120.0, -35.0, 110.0, 256, 144, /*dji=*/false);
    for (const Mix& m : mixes) {
        INFO(depthName(m.srcDepth) << " " << orderName(m.srcOrder) << " -> " << depthName(m.dstDepth) << " "
                                   << orderName(m.dstOrder) << " " << levelsName(m.levels));
        TestImage source = makeImage(OfxRectI{0, 0, 400, 200}, m.srcDepth, m.srcOrder);
        paintPanorama(source);
        TestImage gpu = makeImage(outBounds, m.dstDepth, m.dstOrder);
        TestImage ref = gpu;
        HostTarget target;
        target.image = gpu.view();
        target.window = outBounds;
        target.frame = outBounds;
        target.levels = m.levels;
        std::string error;
        const bool ok = frameHostImage(params, source.view(), target, 0, nullptr, error);
        INFO(error);
        REQUIRE(ok);
        const Promoted promoted = promote(source);
        referenceView(params, promoted.source, promoted.row0, target, ref);
        checkClose(gpu, ref, outBounds);
    }
}

TEST_CASE("large frames stream through many upload and readback bands", "[ofxgpu][cuda]") {
    REQUIRE_CUDA();
    osv::ThreadPool pool(8);
    // A 4096 x 2048 float source is 128 MiB (16 upload bands); a 1920 x 1080
    // float view is 32 MiB (4 readback bands) - both rings wrap many times.
    TestImage source = makeImage(OfxRectI{0, 0, 4096, 2048}, HostDepth::Float, HostOrder::Bgra, true, 0);
    paintPanorama(source);
    const OfxRectI outBounds{0, 0, 1920, 1080};
    const OsvReframeParams params = camera(15.0, 5.0, 80.0, 1920, 1080);
    TestImage gpu = makeImage(outBounds, HostDepth::Float, HostOrder::Bgra, false, 64);
    TestImage ref = gpu;
    HostTarget target;
    target.image = gpu.view();
    target.window = outBounds;
    target.frame = outBounds;
    std::string error;
    const bool ok = frameHostImage(params, source.view(), target, 0, &pool, error);
    INFO(error);
    REQUIRE(ok);
    const Promoted promoted = promote(source);
    referenceView(params, promoted.source, promoted.row0, target, ref);
    checkClose(gpu, ref, outBounds);
}

// ===========================================================================
//  The generator: a sphere already in VRAM
// ===========================================================================

TEST_CASE("a view of a device sphere matches the CPU framing in every format and level", "[ofxgpu][cuda]") {
    REQUIRE_CUDA();
    DeviceSphere sphere(1024, 512);
    osv::ThreadPool pool(4);
    const OfxRectI frame{0, 0, 480, 270};
    const OsvReframeParams params = camera(40.0, -10.0, 80.0, 480, 270);
    const OsvRgbaSource cpuSource = sphere.source();
    for (const HostDepth depth : {HostDepth::Byte, HostDepth::Float}) {
        for (const HostOrder order : {HostOrder::Rgba, HostOrder::Bgra}) {
            for (const OutputLevels levels : {OutputLevels::Full, OutputLevels::Studio}) {
                for (const LayoutCase& layout : kLayouts) {
                    for (const WindowCase& wc : windowsFor(frame)) {
                        INFO(depthName(depth) << " " << orderName(order) << " " << levelsName(levels) << ", "
                                              << layout.name << ", " << wc.name);
                        TestImage gpu = makeImage(frame, depth, order, layout.negative, layout.padBytes);
                        TestImage ref = gpu;
                        HostTarget target;
                        target.image = gpu.view();
                        target.window = wc.window;
                        target.frame = frame;
                        target.levels = levels;
                        std::string error;
                        const bool ok = frameDeviceImage(params, sphere.rgba(), target, &pool, error);
                        INFO(error);
                        REQUIRE(ok);
                        referenceView(params, cpuSource, sphere.host.data(), target, ref);
                        checkClose(gpu, ref, wc.window);
                    }
                }
            }
        }
    }
}

TEST_CASE("the 360 equirect of a device sphere is packed exactly", "[ofxgpu][cuda]") {
    REQUIRE_CUDA();
    // The sphere rendered at the frame's size, as the generator asks for it.
    const OfxRectI frame{0, 0, 640, 320};
    DeviceSphere sphere(640, 320);
    for (const HostDepth depth : {HostDepth::Byte, HostDepth::Float}) {
        for (const HostOrder order : {HostOrder::Rgba, HostOrder::Bgra}) {
            for (const OutputLevels levels : {OutputLevels::Full, OutputLevels::Studio}) {
                for (const LayoutCase& layout : kLayouts) {
                    for (const WindowCase& wc : windowsFor(frame)) {
                        INFO(depthName(depth) << " " << orderName(order) << " " << levelsName(levels) << ", "
                                              << layout.name << ", " << wc.name);
                        TestImage gpu = makeImage(frame, depth, order, layout.negative, layout.padBytes);
                        TestImage ref = gpu;
                        HostTarget target;
                        target.image = gpu.view();
                        target.window = wc.window;
                        target.frame = frame;
                        target.levels = levels;
                        std::string error;
                        const bool ok = packDeviceImage(sphere.rgba(), target, nullptr, error);
                        INFO(error);
                        REQUIRE(ok);
                        referenceEquirect(sphere.host, sphere.w, sphere.h, target, ref);
                        // A copy with the host's own levels and quantisation:
                        // every byte identical.
                        CHECK(gpu.storage == ref.storage);
                    }
                }
            }
        }
    }
}

TEST_CASE("a sphere outside the device's primary context is refused and nothing is written", "[ofxgpu][cuda]") {
    REQUIRE_CUDA();
    popAll();
    CUcontext privateContext = nullptr;
    DeviceSphere sphere(256, 128, /*inPrivateContext=*/true, &privateContext);
    const OfxRectI frame{0, 0, 128, 72};
    TestImage out = makeImage(frame, HostDepth::Byte, HostOrder::Bgra);
    const std::vector<unsigned char> untouched = out.storage;
    HostTarget target;
    target.image = out.view();
    target.window = frame;
    target.frame = frame;
    std::string error;
    CHECK_FALSE(frameDeviceImage(camera(0.0, 0.0, 90.0, 128, 72), sphere.rgba(), target, nullptr, error));
    CHECK_FALSE(error.empty());
    CHECK(out.storage == untouched);
    // And the failure left the thread as it was: nothing current.
    CHECK(currentContext() == nullptr);
}

// ===========================================================================
//  Contexts and threads
// ===========================================================================

TEST_CASE("every call leaves the thread's CUDA context exactly as it found it", "[ofxgpu][cuda]") {
    REQUIRE_CUDA();
    DeviceSphere sphere(512, 256);
    TestImage source = makeImage(OfxRectI{0, 0, 256, 128}, HostDepth::Byte, HostOrder::Bgra);
    paintPanorama(source);
    const OfxRectI frame{0, 0, 160, 90};
    const OsvReframeParams params = camera(10.0, 0.0, 90.0, 160, 90);

    const auto renderAll = [&] {
        TestImage out = makeImage(frame, HostDepth::Float, HostOrder::Rgba);
        HostTarget target;
        target.image = out.view();
        target.window = frame;
        target.frame = frame;
        std::string error;
        CHECK(frameHostImage(params, source.view(), target, 0, nullptr, error));
        CHECK(frameDeviceImage(params, sphere.rgba(), target, nullptr, error));
        target.levels = OutputLevels::Studio;
        DeviceSphere framed(160, 90);
        CHECK(packDeviceImage(framed.rgba(), target, nullptr, error));
    };

    SECTION("nothing current") {
        popAll();
        renderAll();
        CHECK(currentContext() == nullptr);
    }
    SECTION("a host's own context current") {
        popAll();
        CUdevice device = 0;
        REQUIRE(cuDeviceGet(&device, 0) == CUDA_SUCCESS);
        CUcontext hostContext = nullptr;
        // cuCtxCreate makes it current: a host's render thread.
        REQUIRE(cuCtxCreate(&hostContext, 0, device) == CUDA_SUCCESS);
        renderAll();
        CHECK(currentContext() == hostContext);
        popAll();
        cuCtxDestroy(hostContext);
    }
}

TEST_CASE("concurrent filter renders from several threads share the device's slots", "[ofxgpu][cuda]") {
    REQUIRE_CUDA();
    // More threads than slots: some renders wait for a slot, all must finish
    // with their own picture (a shared buffer would mix pans between them).
    constexpr int kThreads = 6;
    constexpr int kFramesEach = 4;
    static_assert(kThreads > kMaxSlotsPerDevice, "the test must oversubscribe the slots");
    osv::ThreadPool pool(4);
    std::atomic<int> failures{0};
    std::atomic<int> rendered{0};
    std::vector<std::string> errors(kThreads);

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            // Each thread its own formats, as VEGAS's clones of one filter
            // would have in different projects.
            const HostDepth depth = (t % 2) ? HostDepth::Byte : HostDepth::Float;
            const HostOrder order = (t % 3) ? HostOrder::Bgra : HostOrder::Rgba;
            TestImage source = makeImage(OfxRectI{0, 0, 600, 300}, depth, order, t % 2 == 0, 0);
            paintPanorama(source);
            const Promoted promoted = promote(source);
            const OfxRectI frame{0, 0, 240, 136};
            for (int f = 0; f < kFramesEach; ++f) {
                OsvReframeParams params{};
                if (!cameraInto(30.0 * t + 7.0 * f, 4.0 * f - 6.0, 70.0 + 5.0 * t, 240, 136, true, params)) {
                    errors[static_cast<std::size_t>(t)] = "no camera";
                    ++failures;
                    continue;
                }
                TestImage gpu = makeImage(frame, depth, order, f % 2 == 1, (f % 3) * 16);
                TestImage ref = gpu;
                HostTarget target;
                target.image = gpu.view();
                target.window = frame;
                target.frame = frame;
                std::string error;
                // Half the renders copy on the shared pool, half on their own
                // thread: both must be safe side by side.
                if (!frameHostImage(params, source.view(), target, 0, (f % 2) ? &pool : nullptr, error)) {
                    errors[static_cast<std::size_t>(t)] = error;
                    ++failures;
                    continue;
                }
                referenceView(params, promoted.source, promoted.row0, target, ref);
                const Diff d = compare(gpu, ref, frame);
                const bool close = !d.outsideTouched && !d.nonFinite &&
                                   (depth == HostDepth::Byte ? d.maxCode <= 1 : d.psnr >= 60.0);
                if (!close) {
                    errors[static_cast<std::size_t>(t)] = "frame " + std::to_string(f) + " differs from its reference";
                    ++failures;
                    continue;
                }
                ++rendered;
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }
    for (int t = 0; t < kThreads; ++t) {
        INFO("thread " << t << ": " << errors[static_cast<std::size_t>(t)]);
        CHECK(errors[static_cast<std::size_t>(t)].empty());
    }
    CHECK(failures.load() == 0);
    CHECK(rendered.load() == kThreads * kFramesEach);
    // The pool never grew past its bound.
    const int slots = devicePoolSlots(0);
    CHECK(slots >= 1);
    CHECK(slots <= kMaxSlotsPerDevice);
}
