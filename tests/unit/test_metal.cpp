// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Metal renderer tests (macOS).  Everything here runs on synthetic lens
// frames (SynthFisheye.h), so a Mac without the sample clip - a CI runner -
// still proves that the shared shader compiled as the Metal Shading Language
// renders what the CPU reference renders.  On a Mac without a usable Metal
// device every case SKIPs.  The sample-clip parity test lives with the CUDA
// and OpenCL ones in test_render.cpp.

#include <catch2/catch_test_macros.hpp>

#include "SynthFisheye.h"

#include "osv/core/Math.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/Renderer.h"

#if defined(OSV_HAVE_METAL)
#include "osv/render/MetalRenderer.h"
#endif

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::testsynth;

#if defined(OSV_HAVE_METAL)

namespace {

/// Stream size of the synthetic lenses (small enough to synthesise quickly).
constexpr int kStream = 768;

/// A scene with smooth structure everywhere: sinusoids in longitude and
/// latitude, tinted per channel so a channel swap cannot hide.  Smooth on
/// purpose for the parity bar: across a HARD edge a last-ulp difference in a
/// sample coordinate moves a bilinear tap from one side to the other and
/// turns into several codes on that pixel (measured: 0.1 % of an equirect of
/// the checker below differ by more than 2 codes, at 108 dB PSNR), which is
/// a property of the edge, not of the backend.
void smoothScene(int, const Vec3d& d, double, double rgb[3]) {
    const double lon = std::atan2(d.x, d.y);
    const double lat = std::asin(std::clamp(d.z, -1.0, 1.0));
    const double v = 0.30 + 0.12 * std::sin(3.0 * lon) * std::cos(2.0 * lat) + 0.08 * std::cos(5.0 * lat + lon);
    rgb[0] = v * 1.10;
    rgb[1] = v;
    rgb[2] = v * 0.85;
}

/// A scene with hard edges everywhere: a longitude / latitude checker in the
/// body frame, tinted like the smooth one.
void tintedChecker(int, const Vec3d& d, double, double rgb[3]) {
    const double lon = std::atan2(d.x, d.y);
    const double lat = std::asin(std::clamp(d.z, -1.0, 1.0));
    const int a = static_cast<int>(std::floor(rad2deg(lon) / 9.0));
    const int b = static_cast<int>(std::floor(rad2deg(lat) / 9.0));
    const double v = ((a + b) & 1) ? 0.55 : 0.12;
    rgb[0] = v * 1.10;
    rgb[1] = v;
    rgb[2] = v * 0.85;
}

/// Create the renderer or SKIP when this Mac has no Metal device.
std::unique_ptr<render::MetalRenderer> metalOrSkip() {
    std::string reason;
    if (!render::MetalRenderer::available(&reason)) {
        SKIP("Metal unavailable: " << reason);
    }
    auto r = render::MetalRenderer::create(0);
    INFO((r.ok() ? std::string() : r.error().message));
    REQUIRE(r.ok());
    return std::move(r).value();
}

/// CPU vs Metal on one job, to the same bar as the CUDA / OpenCL parity tests.
void requireParity(render::IRenderer& gpu, const render::RenderJob& job, ThreadPool& pool, const char* label) {
    render::CpuRenderer cpu(pool);
    auto ref = cpu.render(job);
    REQUIRE(ref.ok());
    auto test = gpu.render(job);
    INFO(label << ": " << (test.ok() ? std::string("ok") : test.error().message));
    REQUIRE(test.ok());
    const render::ImageDiffStats stats = render::compareImages16(ref.value(), test.value());
    INFO(label << ": PSNR " << stats.psnrDb << " dB, max " << stats.maxAbsCode << " codes, within2 "
               << stats.fractionWithin2);
    CHECK(stats.psnrDb >= 60.0);
    CHECK(stats.fractionWithin2 >= 0.9995);
    CHECK(stats.maxAbsCode <= 64);
}

}  // namespace

TEST_CASE("Metal names its device and builds its kernel library", "[render][metal]") {
    auto metal = metalOrSkip();
    CHECK(std::string(metal->name()) == "metal");
    CHECK(render::MetalRenderer::deviceCount() >= 1);
    CHECK(render::MetalRenderer::deviceName(0) != "unknown");
    CHECK(render::MetalRenderer::deviceName(-1) == "unknown");
    INFO("library: " << (metal->usesPrecompiledLibrary() ? "precompiled metallib" : "run-time compile")
                     << ", log: " << metal->buildLog());
    SUCCEED();
    // An index past the end is an argument error, not a crash.
    CHECK(render::MetalRenderer::create(render::MetalRenderer::deviceCount()).error().code ==
          ErrorCode::InvalidArgument);
}

TEST_CASE("Metal renders the synthetic scene like the CPU reference", "[render][metal]") {
    auto metal = metalOrSkip();
    auto rig = makeSyntheticRig(kStream);
    REQUIRE(rig.ok());
    ThreadPool pool(4);
    const SynthPair scene = synthPair(rig.value(), smoothScene, pool);

    // Equirect (both layouts) and three virtual cameras, through every
    // output transfer the kernel implements.
    for (const color::OutputTransfer t : {color::OutputTransfer::PQ, color::OutputTransfer::HLG,
                                          color::OutputTransfer::Rec709, color::OutputTransfer::Linear,
                                          color::OutputTransfer::Passthrough}) {
        const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, t, 0.0f);
        for (const geom::EquirectLayout layout : {geom::EquirectLayout::Standard, geom::EquirectLayout::PolarAxis}) {
            geom::EquirectMap map;
            map.layout = layout;
            map.w = 1024;
            map.h = 512;
            auto job = render::RenderParamsBuilder().rig(rig.value()).equirect(map).color(cp).build(scene.pair);
            REQUIRE(job.ok());
            requireParity(*metal, job.value(), pool, "equirect");
        }
        for (const geom::Projection projection :
             {geom::Projection::Rectilinear, geom::Projection::Stereographic, geom::Projection::EyeOffset}) {
            geom::VirtualCamera cam;
            cam.projection = projection;
            cam.eyeOffset = 0.5;
            cam.w = 640;
            cam.h = 360;
            cam.hfovDeg = projection == geom::Projection::Rectilinear ? 100.0 : 200.0;
            cam.yawDeg = 80.0;  // across the seam
            cam.pitchDeg = 10.0;
            auto job = render::RenderParamsBuilder().rig(rig.value()).camera(cam).color(cp).build(scene.pair);
            REQUIRE(job.ok());
            requireParity(*metal, job.value(), pool, "camera");
        }
    }

    // Hard edges: the same bar the seam tools' GPU test holds every backend
    // to on its hard-edged scene - the image as a whole at >= 60 dB, no pixel
    // off by more than 64 codes.
    const SynthPair edges = synthPair(rig.value(), tintedChecker, pool);
    const OsvColorParams pq = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    geom::EquirectMap map;
    map.w = 1024;
    map.h = 512;
    auto job = render::RenderParamsBuilder().rig(rig.value()).equirect(map).color(pq).build(edges.pair);
    REQUIRE(job.ok());
    render::CpuRenderer cpu(pool);
    auto ref = cpu.render(job.value());
    auto test = metal->render(job.value());
    REQUIRE(ref.ok());
    REQUIRE(test.ok());
    const render::ImageDiffStats stats = render::compareImages16(ref.value(), test.value());
    INFO("checker: PSNR " << stats.psnrDb << " dB, max " << stats.maxAbsCode << " codes, within2 "
                          << stats.fractionWithin2);
    CHECK(stats.psnrDb >= 60.0);
    CHECK(stats.maxAbsCode <= 64);
}

TEST_CASE("Metal renders frame after frame, and changing sizes, without drift", "[render][metal]") {
    auto metal = metalOrSkip();
    auto rig = makeSyntheticRig(kStream);
    REQUIRE(rig.ok());
    ThreadPool pool(4);
    const SynthPair scene = synthPair(rig.value(), tintedChecker, pool);
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);

    // Buffers grow and are reused: a small, a large and the small size again
    // must each render exactly what they rendered the first time.
    std::vector<render::ImageRGBAf> first;
    for (const int w : {256, 1024, 256}) {
        geom::EquirectMap map;
        map.w = w;
        map.h = w / 2;
        auto job = render::RenderParamsBuilder().rig(rig.value()).equirect(map).color(cp).build(scene.pair);
        REQUIRE(job.ok());
        auto a = metal->render(job.value());
        auto b = metal->render(job.value());
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        CHECK(a.value().data == b.value().data);
        first.push_back(std::move(a).value());
    }
    CHECK(first[0].data == first[2].data);
}

TEST_CASE("Metal refuses invalid jobs and CUDA device frames", "[render][metal]") {
    auto metal = metalOrSkip();
    render::RenderJob empty;
    CHECK(metal->render(empty).error().code == ErrorCode::InvalidArgument);

    auto rig = makeSyntheticRig(kStream);
    REQUIRE(rig.ok());
    ThreadPool pool(2);
    const SynthPair scene = synthPair(rig.value(), tintedChecker, pool);
    geom::EquirectMap map;
    map.w = 128;
    map.h = 64;
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    auto job = render::RenderParamsBuilder().rig(rig.value()).equirect(map).color(cp).build(scene.pair);
    REQUIRE(job.ok());
    render::RenderJob onDevice = job.value();
    onDevice.planesOnDevice[0] = true;
    CHECK(metal->render(onDevice).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("makeRenderer picks Metal by name and for auto", "[render][metal]") {
    ThreadPool pool(2);
    std::string chosen;
    auto named = render::makeRenderer("metal", pool, &chosen);
    if (!render::MetalRenderer::available(nullptr)) {
        CHECK_FALSE(named.ok());
        SKIP("no Metal device");
    }
    REQUIRE(named.ok());
    CHECK(chosen == "metal");
    auto best = render::makeRenderer("auto", pool, &chosen);
    REQUIRE(best.ok());
    CHECK(chosen == "metal");  // no CUDA on a Mac: Metal is the best there is
}

#else

TEST_CASE("makeRenderer reports Metal as not compiled in off macOS", "[render][metal]") {
    ThreadPool pool(1);
    auto r = render::makeRenderer("metal", pool, nullptr);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Unsupported);
}

#endif
