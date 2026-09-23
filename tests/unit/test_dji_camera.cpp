// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_dji_camera.cpp - DJI's reframe camera in the library and the kernel.
//
// DJI Studio and DJI's Premiere plug-in render a panorama as a unit sphere
// seen by a perspective camera placed behind its centre: "FOV" is the
// camera's vertical pinhole field of view, "Correction Angle" the eye's
// distance behind the centre, and "Zoom" a read-out derived from the two
// (docs/research/DJI_CAMERA.md).  These tests pin that model three ways:
//
//   * against DJI's own numbers - the Zoom DJI Studio printed for two
//     settings of the same clip, and the preset table;
//   * against closed forms - rectilinear at eye 0, stereographic at eye 1,
//     the visible-angle map theta = alpha + asin(e sin alpha) - through the
//     double-precision reference (geom::DjiSphereCamera);
//   * the float kernel (osvDjiSphereRay, OSV_PROJ_DJI_SPHERE) against that
//     reference, and the CUDA build of the reframe entry point against the
//     CPU one (>= 60 dB, the backends' parity bar).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "osv/core/Math.h"
#include "osv/geom/Presets.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/osv_kernel.h"
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaRenderer.h"
#endif

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::geom;
using Catch::Approx;

namespace {

/// DJI Studio's canvas in the field observations: 16:9.
constexpr double kAspect169 = 16.0 / 9.0;

/// Angle (deg) of a view-frame direction from the view axis (+Y).
[[nodiscard]] double offAxisDeg(const Vec3d& d) {
    const double n = d.norm();
    return rad2deg(std::acos(clampd(d.y / n, -1.0, 1.0)));
}

/// The visible-angle map of section 1 of DJI_CAMERA.md, in degrees:
/// theta = alpha + asin(e sin alpha).
[[nodiscard]] double visibleDeg(double alphaDeg, double e) {
    return alphaDeg + rad2deg(std::asin(e * std::sin(deg2rad(alphaDeg))));
}

/// Angle (rad) between two directions, robust at small angles.
[[nodiscard]] double angleBetween(const double a[3], const double b[3]) {
    const double cx = a[1] * b[2] - a[2] * b[1];
    const double cy = a[2] * b[0] - a[0] * b[2];
    const double cz = a[0] * b[1] - a[1] * b[0];
    const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), dot);
}

/// A DJI camera at 1920 x 1080 - the shape of the field observations.
[[nodiscard]] DjiSphereCamera camera1080(double vfovDeg, double eye) {
    DjiSphereCamera c;
    c.w = 1920;
    c.h = 1080;
    c.vfovDeg = vfovDeg;
    c.eyeDistance = eye;
    return c;
}

/// View ray at a CENTRED offset (x right, y up, pixels) of a 1920x1080 DJI
/// camera, through the double-precision reference.  pixelToRay adds the 0.5
/// pixel centre, so the fractional index here lands exactly on the offset.
[[nodiscard]] bool rayAtOffset(const DjiSphereCamera& c, double x, double y, Vec3d& out) {
    const double px = x + 0.5 * c.w - 0.5;
    const double py = 0.5 * c.h - y - 0.5;
    return c.pixelToRay(px, py, out);
}

}  // namespace

// ===========================================================================
//  Zoom: DJI Studio's read-out
// ===========================================================================

TEST_CASE("djiZoomDeg reproduces DJI Studio's two screenshots within its display rounding", "[geom][dji]") {
    // DJI Studio prints FOV to a tenth and Correction to a hundredth, then a
    // Zoom derived from the UNROUNDED values.  So the check is: some
    // correction inside the printed one's rounding interval gives exactly the
    // printed Zoom - which is what makes the formula DJI's rather than merely
    // close to it.
    struct Observation {
        double fov;
        double correction;
        double dijZoom;       // what DJI Studio showed
        double ourNominal;    // the formula at the printed values
    };
    const Observation seen[] = {
        {103.3, 0.67, 207.1, 207.505},  // "a gentle wide fisheye look"
        {150.0, 0.34, 201.7, 202.149},  // "an extreme radial tunnel stretch"
    };
    for (const Observation& o : seen) {
        INFO("FOV " << o.fov << " correction " << o.correction);
        CHECK(djiZoomDeg(o.fov, o.correction, kAspect169) == Approx(o.ourNominal).margin(0.001));
        const double lo = djiZoomDeg(o.fov, o.correction - 0.005, kAspect169);
        const double hi = djiZoomDeg(o.fov, o.correction + 0.005, kAspect169);
        CHECK(lo < o.dijZoom);
        CHECK(o.dijZoom < hi);
    }
}

TEST_CASE("djiZoomDeg is twice the visible half angle of the centre row, guards included", "[geom][dji]") {
    // For an eye inside the sphere DJI's expression equals the geometric
    // one: 2 (alpha_h + asin(e sin alpha_h)), alpha_h = atan(tan(fov/2) A).
    for (const double aspect : {0.5625, 1.0, kAspect169, 2.35}) {
        for (const double fov : {20.0, 60.0, 103.3, 150.0, 170.0}) {
            for (const double e : {0.0, 0.2, 0.5, 0.67, 1.0}) {
                INFO("aspect " << aspect << " fov " << fov << " e " << e);
                const double alphaH = rad2deg(std::atan(std::tan(deg2rad(0.5 * fov)) * aspect));
                CHECK(djiZoomDeg(fov, e, aspect) == Approx(2.0 * visibleDeg(alphaH, e)).margin(1e-9));
            }
        }
    }
    // Eye 0 is a pinhole: Zoom is the plain horizontal field of view.
    CHECK(djiZoomDeg(90.0, 0.0, 1.0) == Approx(90.0).margin(1e-9));

    // DJI's own guards answer 0 - and so do we, rather than a NaN.
    CHECK(djiZoomDeg(60.0, 0.6, 0.0) == 0.0);
    CHECK(djiZoomDeg(60.0, 0.6, -1.0) == 0.0);
    CHECK(djiZoomDeg(0.0, 0.6, kAspect169) == 0.0);
    CHECK(djiZoomDeg(-5.0, 0.6, kAspect169) == 0.0);
    CHECK(djiZoomDeg(60.0, -0.1, kAspect169) == 0.0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(djiZoomDeg(nan, 0.6, kAspect169) == 0.0);
    CHECK(djiZoomDeg(60.0, nan, kAspect169) == 0.0);
    CHECK(djiZoomDeg(60.0, 0.6, nan) == 0.0);

    // Zoom grows with both controls - the monotonicity the Zoom control's
    // inverse (a bisection along DJI's zoom path) relies on.
    double previous = 0.0;
    for (double delta = -0.6; delta <= 0.4; delta += 0.01) {
        const double fov = clampd(60.0 + 130.0 * delta, 20.0, 150.0);
        const double e = clampd(0.6 + delta, 0.0, 1.0);
        const double z = djiZoomDeg(fov, e, kAspect169);
        CHECK(z >= previous);
        previous = z;
    }
}

TEST_CASE("djiPinholeHalfAngleRad inverts the visible-angle map", "[geom][dji]") {
    for (const double e : {0.0, 0.15, 0.5, 0.8, 1.0}) {
        for (double alphaDeg = 1.0; alphaDeg < 89.0; alphaDeg += 4.0) {
            const double theta = deg2rad(visibleDeg(alphaDeg, e));
            INFO("e " << e << " alpha " << alphaDeg);
            CHECK(rad2deg(djiPinholeHalfAngleRad(theta, e)) == Approx(alphaDeg).margin(1e-9));
        }
    }
    // A point at or behind the eye's horizon on the sphere is not visible.
    CHECK(djiPinholeHalfAngleRad(kPi, 0.5) < 0.0);
    CHECK(djiPinholeHalfAngleRad(std::acos(-0.5) + 1e-6, 0.5) < 0.0);
    CHECK(djiPinholeHalfAngleRad(-0.1, 0.5) < 0.0);
    CHECK(djiPinholeHalfAngleRad(std::numeric_limits<double>::quiet_NaN(), 0.5) < 0.0);
}

// ===========================================================================
//  The camera
// ===========================================================================

TEST_CASE("DjiSphereCamera: eye 0 is rectilinear, eye 1 stereographic, eye > 1 a crystal ball", "[geom][dji]") {
    SECTION("eye 0: a plain pinhole") {
        const DjiSphereCamera c = camera1080(90.0, 0.0);
        const double f = c.focalPx();
        REQUIRE(f == Approx(540.0).epsilon(1e-12));  // (H/2) / tan(45)
        for (const double r : {0.0, 100.0, 400.0, 900.0}) {
            Vec3d d;
            REQUIRE(rayAtOffset(c, r, 0.0, d));
            CHECK(offAxisDeg(d) == Approx(rad2deg(std::atan(r / f))).margin(1e-9));
        }
    }
    SECTION("eye 1: stereographic, the sphere angle is twice the pinhole angle") {
        const DjiSphereCamera c = camera1080(138.0, 1.0);  // DJI's Asteroid
        const double f = c.focalPx();
        for (const double r : {50.0, 300.0, 540.0, 960.0}) {
            Vec3d d;
            REQUIRE(rayAtOffset(c, 0.0, r, d));
            CHECK(offAxisDeg(d) == Approx(2.0 * rad2deg(std::atan(r / f))).margin(1e-9));
        }
    }
    SECTION("eye 1.8: a disc of radius asin(1/e) in the pinhole, black around it") {
        const DjiSphereCamera c = camera1080(75.0, 1.8);  // DJI's Crystal Ball
        const double f = c.focalPx();
        const double rimPx = f * std::tan(std::asin(1.0 / 1.8));
        Vec3d d;
        REQUIRE(rayAtOffset(c, 0.0, 0.0, d));
        CHECK(d.y == Approx(1.0).margin(1e-12));  // the centre sees straight ahead
        CHECK(rayAtOffset(c, 0.0, rimPx * 0.999, d));
        CHECK_FALSE(rayAtOffset(c, 0.0, rimPx * 1.001, d));
        CHECK_FALSE(rayAtOffset(c, 960.0, 540.0, d));  // the corners miss
        // Just inside the rim the far intersection is the tangent point,
        // 90 + asin(1/e) degrees off axis.
        REQUIRE(rayAtOffset(c, rimPx * 0.999999, 0.0, d));
        CHECK(offAxisDeg(d) == Approx(90.0 + rad2deg(std::asin(1.0 / 1.8))).margin(0.2));
    }
    SECTION("an invalid camera answers nothing") {
        Vec3d d;
        CHECK_FALSE(camera1080(0.0, 0.5).pixelToRay(10, 10, d));
        CHECK_FALSE(camera1080(180.0, 0.5).pixelToRay(10, 10, d));
        CHECK_FALSE(camera1080(60.0, -0.1).pixelToRay(10, 10, d));
        CHECK_FALSE(camera1080(60.0, kDjiMaxEyeDistance + 1.0).pixelToRay(10, 10, d));
        CHECK_FALSE(camera1080(60.0, 0.5).pixelToRay(std::numeric_limits<double>::quiet_NaN(), 10, d));
        CHECK(camera1080(0.0, 0.5).focalPx() == 0.0);
        CHECK(camera1080(0.0, 0.5).zoomDeg() == 0.0);
    }
}

TEST_CASE("the two observed DJI lenses: radial profiles and why one is gentle and one a tunnel", "[geom][dji]") {
    // The numbers docs/research/DJI_CAMERA.md tabulates: angle off axis at a
    // radius r along the centre row / column of a 1920 x 1080 frame.
    struct Sample {
        double r;
        double thetaDeg;
    };
    struct Lens {
        const char* name;
        double fov;
        double e;
        std::vector<Sample> profile;
        double centreOverTopEdge;  // radial compression centre -> top edge
    };
    const Lens lenses[] = {
        {"103.3 / 0.67 (gentle wide fisheye)",
         103.3,
         0.67,
         {{135.0, 29.182}, {270.0, 53.265}, {405.0, 70.918}, {540.0, 83.348}, {810.0, 98.534}, {960.0, 103.752}},
         2.9},
        {"150 / 0.34 (radial tunnel)",
         150.0,
         0.34,
         {{135.0, 56.427}, {270.0, 79.252}, {405.0, 89.014}, {540.0, 94.173}, {810.0, 99.426}, {960.0, 101.075}},
         18.3},
    };
    for (const Lens& l : lenses) {
        INFO(l.name);
        const DjiSphereCamera c = camera1080(l.fov, l.e);
        for (const Sample& s : l.profile) {
            INFO("r " << s.r);
            Vec3d d;
            REQUIRE(rayAtOffset(c, s.r, 0.0, d));
            CHECK(offAxisDeg(d) == Approx(s.thetaDeg).margin(0.002));
        }
        // Degrees per pixel at the centre and at the top edge, numerically.
        const auto degPerPx = [&](double r) {
            Vec3d a;
            Vec3d b;
            REQUIRE(rayAtOffset(c, 0.0, r - 0.5, a));
            REQUIRE(rayAtOffset(c, 0.0, r + 0.5, b));
            return offAxisDeg(b) - offAxisDeg(a);
        };
        const double centre = offAxisDeg([&] {
            Vec3d d;
            REQUIRE(rayAtOffset(c, 0.0, 1.0, d));
            return d;
        }());
        CHECK(centre > 0.0);
        CHECK(degPerPx(539.0) > 0.0);
        CHECK(centre / degPerPx(539.0) == Approx(l.centreOverTopEdge).margin(0.1));
        // DJI's Zoom for the camera's own shape is twice the side-edge angle.
        CHECK(c.zoomDeg() == Approx(djiZoomDeg(l.fov, l.e, kAspect169)).margin(1e-9));
    }
    // The first lens spends 2.4x fewer degrees per pixel at the centre than
    // the second: that is the plane "mid-size" versus "tiny in the centre".
    const double gentle = offAxisDeg([&] {
        Vec3d d;
        REQUIRE(rayAtOffset(camera1080(103.3, 0.67), 1.0, 0.0, d));
        return d;
    }());
    const double tunnel = offAxisDeg([&] {
        Vec3d d;
        REQUIRE(rayAtOffset(camera1080(150.0, 0.34), 1.0, 0.0, d));
        return d;
    }());
    CHECK(tunnel / gentle == Approx(2.37).margin(0.02));
}

// ===========================================================================
//  The kernel
// ===========================================================================

TEST_CASE("osvDjiSphereRay matches the double-precision camera to float precision", "[render][kernel][dji]") {
    struct LensCase {
        double fov;
        double e;
    };
    const LensCase cases[] = {{103.3, 0.67}, {150.0, 0.34}, {60.0, 0.6}, {138.0, 1.0}, {75.0, 1.8}, {20.0, 0.0}};
    for (const LensCase& lc : cases) {
        INFO("fov " << lc.fov << " e " << lc.e);
        const DjiSphereCamera c = camera1080(lc.fov, lc.e);
        const float focal = static_cast<float>(c.focalPx());
        double worst = 0.0;
        int agreedMisses = 0;
        int disagreed = 0;
        for (int py = 0; py < c.h; py += 27) {
            for (int px = 0; px < c.w; px += 32) {
                Vec3d ref;
                const bool haveRef = c.pixelToRay(px, py, ref);
                const float nx = static_cast<float>((px + 0.5) - 0.5 * c.w);
                const float ny = static_cast<float>(0.5 * c.h - (py + 0.5));
                float k[3] = {0.0f, 0.0f, 0.0f};
                const int haveKernel = osvDjiSphereRay(focal, static_cast<float>(lc.e), nx, ny, k);
                if (!haveRef || !haveKernel) {
                    // A disagreement is only tolerable a hair from the rim of
                    // a crystal ball, where the discriminant is ~0.
                    if (haveRef != (haveKernel != 0)) {
                        ++disagreed;
                    } else {
                        ++agreedMisses;
                    }
                    continue;
                }
                const double a[3] = {ref.x, ref.y, ref.z};
                const double b[3] = {k[0], k[1], k[2]};
                worst = std::max(worst, angleBetween(a, b));
            }
        }
        INFO("worst angle " << worst << " rad, misses " << agreedMisses << ", disagreements " << disagreed);
        CHECK(worst < 5e-6);  // about one arc-second
        CHECK(disagreed == 0);
        if (lc.e <= 1.0) {
            CHECK(agreedMisses == 0);  // an eye inside the sphere sees everywhere
        } else {
            CHECK(agreedMisses > 0);   // the crystal ball's black surround
        }
    }
    // Garbage in, no ray out.
    float d[3];
    CHECK(osvDjiSphereRay(0.0f, 0.5f, 1.0f, 1.0f, d) == 0);
    CHECK(osvDjiSphereRay(-10.0f, 0.5f, 1.0f, 1.0f, d) == 0);
    CHECK(osvDjiSphereRay(500.0f, -0.5f, 1.0f, 1.0f, d) == 0);
    CHECK(osvDjiSphereRay(std::numeric_limits<float>::quiet_NaN(), 0.5f, 1.0f, 1.0f, d) == 0);
    CHECK(osvDjiSphereRay(500.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f, 1.0f, d) == 0);
}

TEST_CASE("osvViewRay dispatches OSV_PROJ_DJI_SPHERE and matches the eye-offset map inside the sphere",
          "[render][kernel][dji]") {
    // For an eye inside the sphere DJI's camera and the Classic eye-offset
    // projection are the same map, parameterised differently: a pinhole of
    // focal F at distance e behind the centre is the eye-offset projection
    // with f = F / (1 + e).  The two independent implementations (an
    // intersection and an angle inversion) must agree.
    for (const double e : {0.0, 0.34, 0.67, 1.0}) {
        const DjiSphereCamera c = camera1080(103.3, e);
        const float F = static_cast<float>(c.focalPx());
        const float fEye = static_cast<float>(c.focalPx() / (1.0 + e));
        double worst = 0.0;
        for (int py = 0; py < c.h; py += 45) {
            for (int px = 0; px < c.w; px += 60) {
                const float nx = static_cast<float>((px + 0.5) - 0.5 * c.w);
                const float ny = static_cast<float>(0.5 * c.h - (py + 0.5));
                float dji[3];
                float eye[3];
                REQUIRE(osvViewRay(OSV_PROJ_DJI_SPHERE, F, static_cast<float>(e), 0.0f, 0.0f, 1920.0f, 1080.0f, nx,
                                   ny, dji) == 1);
                REQUIRE(osvViewRay(OSV_PROJ_EYE_OFFSET, fEye, static_cast<float>(e), 0.0f, 0.0f, 1920.0f, 1080.0f,
                                   nx, ny, eye) == 1);
                const double a[3] = {dji[0], dji[1], dji[2]};
                const double b[3] = {eye[0], eye[1], eye[2]};
                worst = std::max(worst, angleBetween(a, b));
            }
        }
        INFO("e " << e << ": worst " << worst << " rad");
        CHECK(worst < 1e-4);
    }
}

// ===========================================================================
//  The presets
// ===========================================================================

TEST_CASE("DJI's preset table, its per-shape columns and its lookup", "[geom][dji][presets]") {
    // DJI's Premiere plug-in presets, landscape / 9:16 / 3:4.
    const DjiPreset* crystal = findDjiPreset("Crystal Ball");
    const DjiPreset* asteroid = findDjiPreset("asteroid");
    const DjiPreset* wide = findDjiPreset("wide");
    const DjiPreset* ultra = findDjiPreset("ULTRA_WIDE");
    const DjiPreset* dewarp = findDjiPreset("dewarping");
    REQUIRE(crystal);
    REQUIRE(asteroid);
    REQUIRE(wide);
    REQUIRE(ultra);
    REQUIRE(dewarp);
    CHECK(findDjiPreset("nope") == nullptr);
    CHECK(findDjiPreset("") == nullptr);

    CHECK(crystal->eyeDistance == 1.8);
    CHECK(asteroid->eyeDistance == 1.0);
    CHECK(asteroid->pitchDeg == -90.0);
    CHECK(wide->eyeDistance == 0.6);
    CHECK(ultra->eyeDistance == 0.5);
    CHECK(dewarp->eyeDistance == 0.2);

    // DJI Studio's own list is the landscape column.
    CHECK(djiPresetVfovDeg(*asteroid, kAspect169) == 138.0);
    CHECK(djiPresetVfovDeg(*ultra, kAspect169) == 78.0);
    CHECK(djiPresetVfovDeg(*wide, kAspect169) == 60.0);
    CHECK(djiPresetVfovDeg(*dewarp, kAspect169) == 80.0);
    CHECK(djiPresetVfovDeg(*crystal, kAspect169) == 75.0);
    // Square and 2.35:1 are landscape too; portrait picks the nearer column.
    CHECK(djiPresetVfovDeg(*wide, 1.0) == 60.0);
    CHECK(djiPresetVfovDeg(*wide, 2.35) == 60.0);
    CHECK(djiPresetVfovDeg(*wide, 9.0 / 16.0) == 90.0);
    CHECK(djiPresetVfovDeg(*wide, 0.5) == 90.0);
    CHECK(djiPresetVfovDeg(*wide, 0.75) == 72.0);
    CHECK(djiPresetVfovDeg(*wide, 2.0 / 3.0) == 72.0);
    // A shape nobody can read falls back to landscape.
    CHECK(djiPresetVfovDeg(*wide, 0.0) == 60.0);
    CHECK(djiPresetVfovDeg(*wide, std::numeric_limits<double>::quiet_NaN()) == 60.0);

    // Every preset's Zoom on 16:9, as DJI Studio would print it.
    CHECK(djiZoomDeg(60.0, 0.6, kAspect169) == Approx(142.397).margin(0.001));   // Wide
    CHECK(djiZoomDeg(78.0, 0.5, kAspect169) == Approx(158.921).margin(0.001));   // Ultra Wide
    CHECK(djiZoomDeg(80.0, 0.2, kAspect169) == Approx(131.453).margin(0.001));   // Dewarping
    CHECK(djiZoomDeg(138.0, 1.0, kAspect169) == Approx(311.262).margin(0.001));  // Asteroid
}

// ===========================================================================
//  CUDA parity
// ===========================================================================
#if defined(OSV_HAVE_CUDA)
namespace {

/// A float RGBA Standard-layout equirect labelled by direction: smooth and
/// periodic in longitude, linear in latitude, opaque.
struct Labelled {
    int w = 0;
    int h = 0;
    std::vector<float> rgba;
    OsvRgbaSource src{};
};

[[nodiscard]] Labelled makeLabelledEquirect(int w, int h) {
    Labelled e;
    e.w = w;
    e.h = h;
    e.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    for (int y = 0; y < h; ++y) {
        const double lat = (0.5 - (y + 0.5) / h) * kPi;
        for (int x = 0; x < w; ++x) {
            const double lon = ((x + 0.5) / w - 0.5) * kTwoPi;
            float* p = e.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4u;
            p[0] = static_cast<float>(0.5 + 0.5 * std::cos(lon));
            p[1] = static_cast<float>(0.5 + 0.5 * std::sin(lon));
            p[2] = static_cast<float>(0.5 + lat / kPi);
            p[3] = 1.0f;
        }
    }
    e.src.w = w;
    e.src.h = h;
    e.src.pitchBytes = w * 4 * static_cast<int>(sizeof(float));
    e.src.isHalf = 0;
    e.src.isBgra = 0;
    e.src.flipY = 0;
    return e;
}

/// OsvReframeParams for DJI's lens filling a w x h frame, pointed by
/// (pan, tilt, roll) through the library's own rotation.
[[nodiscard]] OsvReframeParams djiParams(int w, int h, double fov, double e, double pan, double tilt, double roll) {
    OsvReframeParams p;
    std::memset(&p, 0, sizeof(p));
    p.outW = w;
    p.outH = h;
    p.viewW = w;
    p.viewH = h;
    p.projection = OSV_PROJ_DJI_SPHERE;
    DjiSphereCamera c;
    c.w = w;
    c.h = h;
    c.vfovDeg = fov;
    c.eyeDistance = e;
    p.focalPx = static_cast<float>(c.focalPx());
    p.eyeOffset = static_cast<float>(e);
    p.tanHalfH = static_cast<float>(0.5 * w / c.focalPx());
    p.tanHalfV = static_cast<float>(0.5 * h / c.focalPx());
    VirtualCamera rot;
    rot.yawDeg = pan;
    rot.pitchDeg = tilt;
    rot.rollDeg = roll;
    rot.rotation().toFloat9(p.Rout);
    return p;
}

}  // namespace

TEST_CASE("DJI's lens renders identically on the CPU and on CUDA", "[render][reframe][cuda][dji]") {
    std::string reason;
    if (!render::CudaRenderer::available(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    const Labelled pano = makeLabelledEquirect(1440, 720);
    struct LensCase {
        const char* name;
        double fov;
        double e;
    };
    // The two field observations, DJI's Crystal Ball (eye outside the
    // sphere, black surround) and its Asteroid.
    const LensCase cases[] = {
        {"103.3 / 0.67", 103.3, 0.67},
        {"150 / 0.34", 150.0, 0.34},
        {"crystal ball 75 / 1.8", 75.0, 1.8},
        {"asteroid 138 / 1.0", 138.0, 1.0},
    };
    for (const LensCase& lc : cases) {
        INFO(lc.name);
        const OsvReframeParams p = djiParams(960, 540, lc.fov, lc.e, 144.8, -5.9, 0.0);
        auto cpu = render::ImageRGBAf::create(960, 540);
        REQUIRE(cpu.ok());
        for (int y = 0; y < 540; ++y) {
            for (int x = 0; x < 960; ++x) {
                float* out = cpu.value().data.data() + (static_cast<std::size_t>(y) * 960u + x) * 4u;
                osvReframeEquirectPixel(&p, &pano.src, pano.rgba.data(), x, y, out);
            }
        }
        auto gpu = render::cudaReframeEquirect(0, p, pano.src, pano.rgba.data());
        REQUIRE(gpu.ok());
        const render::ImageDiffStats stats = render::compareImages16(cpu.value(), gpu.value());
        INFO("PSNR " << stats.psnrDb << " dB, max diff " << stats.maxAbsCode << " codes");
        CHECK(stats.psnrDb >= 60.0);
        CHECK(stats.alphaIdentical);
        // The centre is always covered; a crystal ball's corner never is.
        CHECK(gpu.value().pixel(480, 270)[3] == 1.0f);
        if (lc.e > 1.0) {
            CHECK(gpu.value().pixel(0, 0)[3] == 0.0f);
            CHECK(cpu.value().pixel(0, 0)[3] == 0.0f);
        }
    }
}
#endif
