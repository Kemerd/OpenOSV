// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for the eye-offset projection and the equirect reframe entry point
// (osvReframeEquirectPixel), the two kernel additions the Premiere effect is
// built on.  Everything here runs on synthetic data; the only optional part
// is the CUDA check, which is tagged [cuda] and skips without a device.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "osv/core/Math.h"
#include "osv/geom/Presets.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/osv_kernel.h"
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaRenderer.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::geom;

namespace {

// -----------------------------------------------------------------------------
//  Helpers: parameter blocks straight from a VirtualCamera
// -----------------------------------------------------------------------------

/// Kernel projection id for a camera projection (reframe cameras only).
int kernelProjection(Projection p) {
    switch (p) {
    case Projection::Rectilinear: return OSV_PROJ_RECTILINEAR;
    case Projection::Fisheye: return OSV_PROJ_FISHEYE;
    case Projection::Stereographic: return OSV_PROJ_STEREOGRAPHIC;
    case Projection::EyeOffset: return OSV_PROJ_EYE_OFFSET;
    case Projection::Equirect: return OSV_PROJ_RECTILINEAR;
    }
    return OSV_PROJ_RECTILINEAR;
}

/// Fisheye-pipeline parameter block with only the ray-generation fields set
/// (enough for osvRayForPixel; no lenses, no colour).
OsvRenderParams rayParams(const VirtualCamera& cam) {
    OsvRenderParams p;
    std::memset(&p, 0, sizeof(p));
    p.mode = OSV_MODE_REFRAME;
    p.outW = cam.w;
    p.outH = cam.h;
    p.projection = kernelProjection(cam.projection);
    p.focalPx = static_cast<float>(cam.focalPx());
    p.eyeOffset = static_cast<float>(cam.eyeOffset);
    const double t = std::tan(deg2rad(0.5 * cam.effectiveHfovDeg()));
    p.tanHalfH = static_cast<float>(t);
    p.tanHalfV = static_cast<float>(t * cam.h / cam.w);
    Mat3d::identity().toFloat9(p.Rout);
    return p;
}

/// Equirect reframe parameter block: the camera describes the viewport, the
/// viewport sits at (vx, vy) inside an outW x outH frame.
OsvReframeParams reframeParams(const VirtualCamera& cam, int outW, int outH, int vx, int vy, bool fillAlphaOne) {
    OsvReframeParams p;
    std::memset(&p, 0, sizeof(p));
    p.outW = outW;
    p.outH = outH;
    p.viewX = vx;
    p.viewY = vy;
    p.viewW = cam.w;
    p.viewH = cam.h;
    p.projection = kernelProjection(cam.projection);
    p.eyeOffset = static_cast<float>(cam.eyeOffset);
    p.focalPx = static_cast<float>(cam.focalPx());
    const double t = std::tan(deg2rad(0.5 * cam.effectiveHfovDeg()));
    p.tanHalfH = static_cast<float>(t);
    p.tanHalfV = static_cast<float>(t * cam.h / cam.w);
    cam.rotation().toFloat9(p.Rout);
    p.fillAlphaOne = fillAlphaOne ? 1 : 0;
    return p;
}

// -----------------------------------------------------------------------------
//  Helpers: the labelled panorama
// -----------------------------------------------------------------------------

/// Longitude / latitude (radians) of the pixel centre (x, y) in the Standard
/// layout.
void lonLatOf(int x, int y, int w, int h, double& lon, double& lat) {
    lon = ((x + 0.5) / w - 0.5) * kTwoPi;
    lat = (0.5 - (y + 0.5) / h) * kPi;
}

/// Colour label of a direction: smooth and periodic in longitude so the
/// +/-180 seam carries no discontinuity of its own, linear in latitude.
void labelColour(double lon, double lat, float out[4]) {
    out[0] = static_cast<float>(0.5 + 0.5 * std::cos(lon));
    out[1] = static_cast<float>(0.5 + 0.5 * std::sin(lon));
    out[2] = static_cast<float>(0.5 + lat / kPi);
    out[3] = 0.75f;
}

/// Label colour of a unit body direction.
void labelOfDirection(const Vec3d& d, float out[4]) {
    const double lon = std::atan2(d.x, d.y);
    const double lat = std::asin(clampd(d.z, -1.0, 1.0));
    labelColour(lon, lat, out);
}

/// A 32-bit float RGBA Standard-layout equirect whose colour is the label.
struct LabelledEquirect {
    int w = 0;
    int h = 0;
    std::vector<float> rgba;  // w * h * 4, tightly packed
    OsvRgbaSource src{};
};

LabelledEquirect makeLabelled(int w, int h) {
    LabelledEquirect e;
    e.w = w;
    e.h = h;
    e.rgba.resize(static_cast<std::size_t>(w) * h * 4u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double lon = 0.0, lat = 0.0;
            lonLatOf(x, y, w, h, lon, lat);
            labelColour(lon, lat, e.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4u);
        }
    }
    e.src.w = w;
    e.src.h = h;
    e.src.pitchBytes = w * 4 * static_cast<int>(sizeof(float));
    e.src.isHalf = 0;
    e.src.isBgra = 0;
    return e;
}

/// IEEE binary32 -> binary16 with round-to-nearest-even (test-side reference,
/// independent of the kernel's decoder).
std::uint16_t floatToHalf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::uint32_t expField = (bits >> 23) & 0xFFu;
    const std::uint32_t mant = bits & 0x7FFFFFu;
    if (expField == 0xFFu) {
        // infinity / NaN (keep NaN-ness)
        return static_cast<std::uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
    }
    const int exp = static_cast<int>(expField) - 127;
    if (exp > 15) {
        return static_cast<std::uint16_t>(sign | 0x7C00u);  // overflow -> inf
    }
    if (exp >= -14) {
        // normal half: 23 -> 10 mantissa bits with RNE
        std::uint32_t halfExp = static_cast<std::uint32_t>(exp + 15);
        std::uint32_t m = mant >> 13;
        const std::uint32_t rem = mant & 0x1FFFu;
        if (rem > 0x1000u || (rem == 0x1000u && (m & 1u))) {
            ++m;
            if (m == 0x400u) {
                m = 0;
                ++halfExp;
                if (halfExp >= 31u) {
                    return static_cast<std::uint16_t>(sign | 0x7C00u);
                }
            }
        }
        return static_cast<std::uint16_t>(sign | (halfExp << 10) | m);
    }
    if (exp < -25) {
        return static_cast<std::uint16_t>(sign);  // below half the smallest subnormal
    }
    // subnormal half: value = full * 2^(exp - 23); half unit is 2^-24
    const std::uint32_t full = mant | 0x800000u;
    const int shift = -exp - 1;  // 14..24
    std::uint32_t m = full >> shift;
    const std::uint32_t rem = full & ((1u << shift) - 1u);
    const std::uint32_t halfway = 1u << (shift - 1);
    if (rem > halfway || (rem == halfway && (m & 1u))) {
        ++m;  // a carry into bit 10 is the smallest normal, encoded correctly as is
    }
    return static_cast<std::uint16_t>(sign | m);
}

/// binary16 -> binary32 reference built from ldexp (no bit tricks).
float halfToFloatRef(std::uint16_t h) {
    const int sign = (h >> 15) & 1;
    const int exp = (h >> 10) & 0x1F;
    const int mant = h & 0x3FF;
    double v = 0.0;
    if (exp == 0) {
        v = std::ldexp(static_cast<double>(mant), -24);
    } else if (exp == 31) {
        v = mant ? std::nan("") : HUGE_VAL;
    } else {
        v = std::ldexp(static_cast<double>(1024 + mant), exp - 25);
    }
    return static_cast<float>(sign ? -v : v);
}

/// 16-bit float copy of a labelled equirect, optionally in BGRA order.
struct HalfEquirect {
    std::vector<std::uint16_t> data;
    OsvRgbaSource src{};
};

HalfEquirect toHalf(const LabelledEquirect& e, bool bgra) {
    HalfEquirect h;
    h.data.resize(e.rgba.size());
    for (std::size_t i = 0; i < e.rgba.size(); i += 4) {
        const float* px = e.rgba.data() + i;
        h.data[i + 0] = floatToHalf(bgra ? px[2] : px[0]);
        h.data[i + 1] = floatToHalf(px[1]);
        h.data[i + 2] = floatToHalf(bgra ? px[0] : px[2]);
        h.data[i + 3] = floatToHalf(px[3]);
    }
    h.src = e.src;
    h.src.pitchBytes = e.w * 4 * static_cast<int>(sizeof(std::uint16_t));
    h.src.isHalf = 1;
    h.src.isBgra = bgra ? 1 : 0;
    return h;
}

/// 32-bit BGRA copy of a labelled equirect.
LabelledEquirect toBgra(const LabelledEquirect& e) {
    LabelledEquirect b = e;
    for (std::size_t i = 0; i < b.rgba.size(); i += 4) {
        std::swap(b.rgba[i], b.rgba[i + 2]);
    }
    b.src.isBgra = 1;
    return b;
}

/// Run the shared per-pixel function over the whole output frame on the CPU.
render::ImageRGBAf reframeCpu(const OsvReframeParams& p, const OsvRgbaSource& src, const void* pixels) {
    auto img = render::ImageRGBAf::create(static_cast<std::uint32_t>(p.outW), static_cast<std::uint32_t>(p.outH));
    REQUIRE(img.ok());
    render::ImageRGBAf out = std::move(img).value();
    for (int y = 0; y < p.outH; ++y) {
        for (int x = 0; x < p.outW; ++x) {
            osvReframeEquirectPixel(&p, &src, pixels, x, y, out.row(static_cast<std::uint32_t>(y)) + x * 4);
        }
    }
    return out;
}

/// Largest absolute channel difference between two same-sized images.
float maxAbsDiff(const render::ImageRGBAf& a, const render::ImageRGBAf& b) {
    REQUIRE(a.data.size() == b.data.size());
    float m = 0.0f;
    for (std::size_t i = 0; i < a.data.size(); ++i) {
        m = std::max(m, std::fabs(a.data[i] - b.data[i]));
    }
    return m;
}

}  // namespace

// -----------------------------------------------------------------------------
//  VirtualCamera: eye-offset model (double precision)
// -----------------------------------------------------------------------------
TEST_CASE("VirtualCamera eye offset: d = 0 is rectilinear, d = 1 is stereographic", "[geom][camera][eye-offset]") {
    REQUIRE(std::string(projectionName(Projection::EyeOffset)) == "EyeOffset");

    // d = 0 against the pinhole on a 16:9 viewport.
    VirtualCamera eye;
    eye.projection = Projection::EyeOffset;
    eye.eyeOffset = 0.0;
    eye.w = 1920;
    eye.h = 1080;
    eye.hfovDeg = 120.0;
    VirtualCamera rect = eye;
    rect.projection = Projection::Rectilinear;
    REQUIRE(eye.isValid());
    REQUIRE_THAT(eye.focalPx(), Catch::Matchers::WithinRel(rect.focalPx(), 1e-12));
    for (int y = 0; y < 1080; y += 97) {
        for (int x = 0; x < 1920; x += 101) {
            Vec3d a, b;
            REQUIRE(eye.pixelToRay(x, y, a));
            REQUIRE(rect.pixelToRay(x, y, b));
            INFO("pixel " << x << "," << y);
            REQUIRE((a - b).norm() < 1e-12);
        }
    }
    // The right edge is exactly hfov / 2 from the axis.
    Vec3d edge;
    REQUIRE(eye.pixelToRay(eye.w - 0.5, 0.5 * eye.h - 0.5, edge));
    REQUIRE_THAT(rad2deg(edge.angleTo(Vec3d{0.0, 1.0, 0.0})), Catch::Matchers::WithinAbs(60.0, 1e-9));

    // d = 1 against the stereographic model on a wide square viewport.
    eye.eyeOffset = 1.0;
    eye.w = 1000;
    eye.h = 1000;
    eye.hfovDeg = 240.0;
    VirtualCamera stereo = eye;
    stereo.projection = Projection::Stereographic;
    REQUIRE(eye.isValid());
    REQUIRE_THAT(eye.focalPx(), Catch::Matchers::WithinRel(stereo.focalPx(), 1e-12));
    for (int y = 0; y < 1000; y += 53) {
        for (int x = 0; x < 1000; x += 47) {
            Vec3d a, b;
            REQUIRE(eye.pixelToRay(x, y, a));
            REQUIRE(stereo.pixelToRay(x, y, b));
            INFO("pixel " << x << "," << y);
            REQUIRE((a - b).norm() < 1e-12);
        }
    }
    REQUIRE(eye.pixelToRay(eye.w - 0.5, 0.5 * eye.h - 0.5, edge));
    REQUIRE_THAT(rad2deg(edge.angleTo(Vec3d{0.0, 1.0, 0.0})), Catch::Matchers::WithinAbs(120.0, 1e-9));
}

TEST_CASE("VirtualCamera eye offset: monotonic theta(r), round trip, FOV clamp and validation",
          "[geom][camera][eye-offset]") {
    for (const double d : {0.0, 0.15, 0.4, 0.5, 0.85, 1.0}) {
        VirtualCamera cam;
        cam.projection = Projection::EyeOffset;
        cam.eyeOffset = d;
        cam.w = 2000;
        cam.h = 2000;
        cam.hfovDeg = 100.0;
        REQUIRE(cam.isValid());
        const double f = cam.focalPx();
        REQUIRE(f > 0.0);
        INFO("d = " << d);

        // theta(r) strictly increases along the +x axis out to the corner.
        double previous = -1.0;
        for (double r = 0.0; r <= 1400.0; r += 7.0) {
            Vec3d dir;
            REQUIRE(cam.pixelToRay(0.5 * cam.w - 0.5 + r, 0.5 * cam.h - 0.5, dir));
            const double theta = dir.angleTo(Vec3d{0.0, 1.0, 0.0});
            REQUIRE(theta > previous);
            previous = theta;
        }

        // Round trip: forward model r(theta) -> pixelToRay -> theta.
        const double thetaMax = std::acos(-d);
        for (double theta = 0.05; theta < thetaMax - 0.05; theta += 0.05) {
            const double r = f * (1.0 + d) * std::sin(theta) / (d + std::cos(theta));
            Vec3d dir;
            REQUIRE(cam.pixelToRay(0.5 * cam.w - 0.5 + r * 0.6, 0.5 * cam.h - 0.5 - r * 0.8, dir));
            REQUIRE_THAT(dir.angleTo(Vec3d{0.0, 1.0, 0.0}), Catch::Matchers::WithinAbs(theta, 1e-9));
            REQUIRE(dir.x > 0.0);
            REQUIRE(dir.z > 0.0);
        }
    }

    // Field of view clamp: 2 acos(-d) minus one degree.
    REQUIRE_THAT(eyeOffsetMaxHfovDeg(0.0), Catch::Matchers::WithinAbs(179.0, 1e-9));
    REQUIRE_THAT(eyeOffsetMaxHfovDeg(1.0), Catch::Matchers::WithinAbs(359.0, 1e-9));
    REQUIRE_THAT(eyeOffsetMaxHfovDeg(0.5), Catch::Matchers::WithinAbs(2.0 * rad2deg(std::acos(-0.5)) - 1.0, 1e-9));
    REQUIRE_THAT(eyeOffsetMaxHfovDeg(7.0), Catch::Matchers::WithinAbs(359.0, 1e-9));  // clamped input
    REQUIRE_THAT(eyeOffsetMaxHfovDeg(std::nan("")), Catch::Matchers::WithinAbs(179.0, 1e-9));
    VirtualCamera wide;
    wide.projection = Projection::EyeOffset;
    wide.eyeOffset = 0.0;
    wide.w = 1920;
    wide.h = 1080;
    wide.hfovDeg = 300.0;
    REQUIRE(wide.isValid());
    REQUIRE_THAT(wide.effectiveHfovDeg(), Catch::Matchers::WithinAbs(179.0, 1e-9));
    Vec3d edge;
    REQUIRE(wide.pixelToRay(wide.w - 0.5, 0.5 * wide.h - 0.5, edge));
    REQUIRE_THAT(rad2deg(edge.angleTo(Vec3d{0.0, 1.0, 0.0})), Catch::Matchers::WithinAbs(89.5, 1e-9));
    // A pixel absurdly far outside the frame reaches the asymptote (atan of
    // an enormous k rounds to pi/2 == acos(-0)): no ray.  Merely large radii
    // stay valid because theta(r) never reaches the limit for finite r.
    Vec3d none;
    REQUIRE_FALSE(wide.pixelToRay(1e300, 0.5 * wide.h - 0.5, none));
    REQUIRE(wide.pixelToRay(1e12, 0.5 * wide.h - 0.5, none));
    REQUIRE(rad2deg(none.angleTo(Vec3d{0.0, 1.0, 0.0})) < 90.0);
    // Other projections are not clamped.
    wide.projection = Projection::Fisheye;
    REQUIRE(wide.effectiveHfovDeg() == 300.0);

    // Validation of the offset itself.
    VirtualCamera bad;
    bad.projection = Projection::EyeOffset;
    bad.eyeOffset = 1.5;
    REQUIRE_FALSE(bad.isValid());
    bad.eyeOffset = -0.1;
    REQUIRE_FALSE(bad.isValid());
    bad.eyeOffset = std::nan("");
    REQUIRE_FALSE(bad.isValid());
    REQUIRE(bad.focalPx() == 0.0);
}

// -----------------------------------------------------------------------------
//  Kernel (float): the same equivalences through osvRayForPixel
// -----------------------------------------------------------------------------
TEST_CASE("kernel eye offset reproduces the rectilinear and stereographic rays to float precision",
          "[render][kernel][eye-offset]") {
    struct Case {
        double d;
        Projection reference;
        int w, h;
        double hfov;
    };
    for (const Case& c : {Case{0.0, Projection::Rectilinear, 1920, 1080, 120.0},
                          Case{0.0, Projection::Rectilinear, 640, 480, 95.0},
                          Case{1.0, Projection::Stereographic, 1000, 1000, 240.0},
                          Case{1.0, Projection::Stereographic, 1280, 720, 300.0}}) {
        VirtualCamera eye;
        eye.projection = Projection::EyeOffset;
        eye.eyeOffset = c.d;
        eye.w = c.w;
        eye.h = c.h;
        eye.hfovDeg = c.hfov;
        VirtualCamera ref = eye;
        ref.projection = c.reference;
        const OsvRenderParams pe = rayParams(eye);
        const OsvRenderParams pr = rayParams(ref);
        REQUIRE(pe.projection == OSV_PROJ_EYE_OFFSET);
        float worst = 0.0f;
        std::size_t rays = 0;
        for (int y = 0; y < c.h; y += 3) {
            for (int x = 0; x < c.w; x += 3) {
                float a[3], b[3];
                const int okA = osvRayForPixel(&pe, static_cast<float>(x), static_cast<float>(y), a);
                const int okB = osvRayForPixel(&pr, static_cast<float>(x), static_cast<float>(y), b);
                REQUIRE(okA == okB);
                if (!okA) {
                    continue;
                }
                for (int i = 0; i < 3; ++i) {
                    worst = std::max(worst, std::fabs(a[i] - b[i]));
                }
                ++rays;
            }
        }
        INFO("d = " << c.d << " " << c.w << "x" << c.h << " hfov " << c.hfov << ": worst |delta| " << worst);
        REQUIRE(rays > 1000);
        REQUIRE(worst < 2e-6f);
    }

    // The scalar inverse: monotonic and bounded by the asymptote.
    for (const float d : {0.0f, 0.3f, 0.5f, 1.0f}) {
        const float limit = std::acos(-d);
        float previous = -1.0f;
        for (float r = 0.0f; r < 5000.0f; r += 5.0f) {
            const float theta = osvEyeOffsetTheta(r, 100.0f, d);
            REQUIRE(theta >= 0.0f);
            REQUIRE(theta > previous);
            REQUIRE(theta < limit);
            previous = theta;
        }
        // Invalid inputs are reported as uncovered, never as NaN.
        REQUIRE(osvEyeOffsetTheta(1.0f, 0.0f, d) < 0.0f);
        REQUIRE(osvEyeOffsetTheta(-1.0f, 100.0f, d) < 0.0f);
        REQUIRE(osvEyeOffsetTheta(std::nanf(""), 100.0f, d) < 0.0f);
        // An absurd radius (k^2 overflows float) must land on the limit
        // acos(-d) (either reported as uncovered or within an ulp of it),
        // never on a wrong smaller angle.
        const float huge = osvEyeOffsetTheta(1e30f, 100.0f, d);
        if (huge >= 0.0f) {
            REQUIRE_THAT(huge, Catch::Matchers::WithinAbs(limit, 1e-6));
        }
        if (d == 0.0f) {
            REQUIRE(huge < 0.0f);
        }
    }
}

// -----------------------------------------------------------------------------
//  Half-float decoder
// -----------------------------------------------------------------------------
TEST_CASE("osvHalfToFloat decodes every binary16 bit pattern", "[render][kernel]") {
    std::size_t finite = 0;
    for (unsigned int bits = 0; bits < 65536u; ++bits) {
        const float got = osvHalfToFloat(bits);
        const float want = halfToFloatRef(static_cast<std::uint16_t>(bits));
        if (std::isnan(want)) {
            REQUIRE(std::isnan(got));
            continue;
        }
        INFO("bits 0x" << std::hex << bits);
        REQUIRE(got == want);  // exact, including signed zero and subnormals
        REQUIRE(std::signbit(got) == std::signbit(want));
        if (std::isfinite(want)) {
            ++finite;
        }
    }
    REQUIRE(finite == 65536u - 2u * 1024u);  // all but the 2048 inf/NaN patterns
    // The test-side encoder round trips through the decoder.
    for (const float v : {0.0f, -0.0f, 1.0f, 0.75f, 65504.0f, 6.1035156e-05f, 5.9604645e-08f, -2.5f}) {
        REQUIRE(osvHalfToFloat(floatToHalf(v)) == v);
    }
}

// -----------------------------------------------------------------------------
//  osvReframeEquirectPixel
// -----------------------------------------------------------------------------
TEST_CASE("equirect reframe lands the expected colours for pan and tilt", "[render][reframe]") {
    const LabelledEquirect pano = makeLabelled(720, 360);

    VirtualCamera cam;
    cam.projection = Projection::EyeOffset;
    cam.eyeOffset = 0.15;
    cam.w = 640;
    cam.h = 360;
    cam.hfovDeg = 90.0;

    struct View {
        double pan, tilt;
        float r, g, b;  // expected label at the centre
    };
    // Conventions: positive pan looks towards -X (longitude -90), positive
    // tilt looks up (latitude +45 -> B = 0.75).
    const View views[] = {
        {0.0, 0.0, 1.0f, 0.5f, 0.5f},     {90.0, 0.0, 0.5f, 0.0f, 0.5f},   {180.0, 0.0, 0.0f, 0.5f, 0.5f},
        {-90.0, 0.0, 0.5f, 1.0f, 0.5f},   {0.0, 45.0, 1.0f, 0.5f, 0.75f},  {0.0, -45.0, 1.0f, 0.5f, 0.25f},
        {90.0, 45.0, 0.5f, 0.0f, 0.75f},  {180.0, -45.0, 0.0f, 0.5f, 0.25f},
    };
    for (const View& v : views) {
        cam.yawDeg = v.pan;
        cam.pitchDeg = v.tilt;
        const OsvReframeParams p = reframeParams(cam, cam.w, cam.h, 0, 0, false);
        INFO("pan " << v.pan << " tilt " << v.tilt);

        // Centre pixel: the hard-coded expectation guards the conventions.
        float centre[4];
        osvReframeEquirectPixel(&p, &pano.src, pano.rgba.data(), cam.w / 2, cam.h / 2, centre);
        REQUIRE_THAT(centre[0], Catch::Matchers::WithinAbs(v.r, 2e-3));
        REQUIRE_THAT(centre[1], Catch::Matchers::WithinAbs(v.g, 2e-3));
        REQUIRE_THAT(centre[2], Catch::Matchers::WithinAbs(v.b, 2e-3));
        REQUIRE(centre[3] == 0.75f);  // straight alpha passes through

        // Every pixel: the double-precision camera predicts the label.
        const Mat3d rot = cam.rotation();
        float worst = 0.0f;
        for (int y = 0; y < cam.h; y += 5) {
            for (int x = 0; x < cam.w; x += 5) {
                Vec3d view;
                REQUIRE(cam.pixelToRay(x, y, view));
                float want[4];
                labelOfDirection(rot * view, want);
                float got[4];
                osvReframeEquirectPixel(&p, &pano.src, pano.rgba.data(), x, y, got);
                for (int i = 0; i < 4; ++i) {
                    worst = std::max(worst, std::fabs(got[i] - want[i]));
                }
            }
        }
        INFO("worst channel error " << worst);
        REQUIRE(worst < 2e-3f);
    }
}

TEST_CASE("equirect reframe is seamless across the +/-180 degree seam", "[render][reframe]") {
    const LabelledEquirect pano = makeLabelled(720, 360);
    VirtualCamera cam;
    cam.projection = Projection::EyeOffset;
    cam.eyeOffset = 0.4;
    cam.w = 800;
    cam.h = 450;
    cam.hfovDeg = 110.0;
    cam.yawDeg = 180.0;  // straight at the seam
    const OsvReframeParams p = reframeParams(cam, cam.w, cam.h, 0, 0, true);
    const render::ImageRGBAf out = reframeCpu(p, pano.src, pano.rgba.data());

    // Looking backwards: R = 0.5 + 0.5 cos(pi) = 0 at the centre.
    const float* centre = out.pixel(static_cast<std::uint32_t>(cam.w / 2), static_cast<std::uint32_t>(cam.h / 2));
    REQUIRE_THAT(centre[0], Catch::Matchers::WithinAbs(0.0, 2e-3));
    REQUIRE_THAT(centre[1], Catch::Matchers::WithinAbs(0.5, 2e-3));
    REQUIRE(centre[3] == 1.0f);

    // Horizontal neighbours never jump: the label changes by at most a few
    // thousandths per output pixel, a wrap bug would produce a step of ~1.
    float worstStep = 0.0f;
    for (std::uint32_t y = 0; y < out.h; ++y) {
        for (std::uint32_t x = 1; x < out.w; ++x) {
            const float* a = out.pixel(x - 1, y);
            const float* b = out.pixel(x, y);
            for (int i = 0; i < 3; ++i) {
                worstStep = std::max(worstStep, std::fabs(a[i] - b[i]));
            }
        }
    }
    INFO("largest horizontal step " << worstStep);
    REQUIRE(worstStep < 0.01f);

    // The exact seam direction (-Y, longitude +/-180) falls between source
    // columns W - 1 and 0; the sampler must blend both through the wrap.
    const float back[3] = {0.0f, -1.0f, 0.0f};
    float exact[4];
    osvSampleEquirectRgba(&pano.src, pano.rgba.data(), back, exact);
    REQUIRE_THAT(exact[0], Catch::Matchers::WithinAbs(0.0, 1e-4));
    REQUIRE_THAT(exact[1], Catch::Matchers::WithinAbs(0.5, 1e-4));
    REQUIRE_THAT(exact[2], Catch::Matchers::WithinAbs(0.5, 1e-4));
    // A direction slightly past the seam on either side gives the same colour
    // (the label is continuous there), proving the wrap is symmetric.
    const float left[3] = {-0.001f, -1.0f, 0.0f};
    const float right[3] = {0.001f, -1.0f, 0.0f};
    float l[4], r[4];
    osvSampleEquirectRgba(&pano.src, pano.rgba.data(), left, l);
    osvSampleEquirectRgba(&pano.src, pano.rgba.data(), right, r);
    REQUIRE_THAT(l[0], Catch::Matchers::WithinAbs(r[0], 1e-4));
    REQUIRE_THAT(l[1], Catch::Matchers::WithinAbs(1.0 - r[1], 2e-3));  // sin is odd about the seam
}

TEST_CASE("equirect reframe: 16f and BGRA sources match the 32f RGBA result", "[render][reframe]") {
    const LabelledEquirect pano = makeLabelled(720, 360);
    const LabelledEquirect bgra = toBgra(pano);
    const HalfEquirect half = toHalf(pano, false);
    const HalfEquirect halfBgra = toHalf(pano, true);

    VirtualCamera cam;
    cam.projection = Projection::EyeOffset;
    cam.eyeOffset = 1.0;
    cam.w = 512;
    cam.h = 512;
    cam.hfovDeg = 240.0;
    cam.yawDeg = 33.0;
    cam.pitchDeg = -20.0;
    cam.rollDeg = 10.0;
    const OsvReframeParams p = reframeParams(cam, cam.w, cam.h, 0, 0, false);

    const render::ImageRGBAf ref = reframeCpu(p, pano.src, pano.rgba.data());
    const render::ImageRGBAf viaBgra = reframeCpu(p, bgra.src, bgra.rgba.data());
    const render::ImageRGBAf viaHalf = reframeCpu(p, half.src, half.data.data());
    const render::ImageRGBAf viaHalfBgra = reframeCpu(p, halfBgra.src, halfBgra.data.data());

    // Channel order is a pure permutation: bit-identical.
    REQUIRE(maxAbsDiff(ref, viaBgra) == 0.0f);
    // Half precision on [0, 1] values is 2^-11: comfortably within 1e-3.
    REQUIRE(maxAbsDiff(ref, viaHalf) < 1e-3f);
    REQUIRE(maxAbsDiff(ref, viaHalfBgra) < 1e-3f);
    REQUIRE(maxAbsDiff(viaHalf, viaHalfBgra) == 0.0f);
    // And the result is not trivially empty.
    REQUIRE(ref.pixel(256, 256)[3] == 0.75f);
}

TEST_CASE("equirect reframe: letterbox viewport, alpha fill and invalid inputs", "[render][reframe]") {
    const LabelledEquirect pano = makeLabelled(360, 180);
    VirtualCamera cam;
    cam.projection = Projection::EyeOffset;
    cam.eyeOffset = 0.15;
    cam.w = 480;   // 4:3 picture...
    cam.h = 360;
    cam.hfovDeg = 100.0;
    const int outW = 640, outH = 360;  // ...inside a 16:9 frame, pillarboxed
    const int vx = 80, vy = 0;

    SECTION("outside the viewport is transparent black, inside is opaque with fillAlphaOne") {
        const OsvReframeParams p = reframeParams(cam, outW, outH, vx, vy, true);
        const render::ImageRGBAf out = reframeCpu(p, pano.src, pano.rgba.data());
        std::size_t inside = 0, outside = 0;
        for (int y = 0; y < outH; ++y) {
            for (int x = 0; x < outW; ++x) {
                const float* px = out.pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                const bool in = x >= vx && x < vx + cam.w && y >= vy && y < vy + cam.h;
                if (in) {
                    REQUIRE(px[3] == 1.0f);
                    ++inside;
                } else {
                    REQUIRE(px[0] == 0.0f);
                    REQUIRE(px[1] == 0.0f);
                    REQUIRE(px[2] == 0.0f);
                    REQUIRE(px[3] == 0.0f);
                    ++outside;
                }
            }
        }
        REQUIRE(inside == static_cast<std::size_t>(cam.w) * cam.h);
        REQUIRE(outside == static_cast<std::size_t>(outW) * outH - inside);
        // The viewport centre looks along +Y regardless of where it sits.
        const float* centre = out.pixel(vx + cam.w / 2, vy + cam.h / 2);
        REQUIRE_THAT(centre[0], Catch::Matchers::WithinAbs(1.0, 5e-3));
        REQUIRE_THAT(centre[1], Catch::Matchers::WithinAbs(0.5, 5e-3));
    }

    SECTION("without fillAlphaOne the source alpha is carried straight") {
        const OsvReframeParams p = reframeParams(cam, outW, outH, vx, vy, false);
        float px[4];
        osvReframeEquirectPixel(&p, &pano.src, pano.rgba.data(), vx + 10, vy + 10, px);
        REQUIRE(px[3] == 0.75f);
        osvReframeEquirectPixel(&p, &pano.src, pano.rgba.data(), vx - 1, vy + 10, px);
        REQUIRE(px[3] == 0.0f);
    }

    SECTION("beyond the valid radius of the projection is uncovered") {
        // With the inverse formula every finite radius stays below the
        // asymptote, so the guard is reached by a degenerate focal length:
        // a vanishing focal makes every off-centre pixel's k enormous and
        // theta lands on acos(-d) itself (d = 0: atan(inf) == acos(0)).
        VirtualCamera flat = cam;
        flat.eyeOffset = 0.0;
        OsvReframeParams p = reframeParams(flat, outW, outH, vx, vy, true);
        float px[4];
        osvReframeEquirectPixel(&p, &pano.src, pano.rgba.data(), vx + 10, vy + 10, px);
        REQUIRE(px[3] == 1.0f);
        p.focalPx = 1e-30f;
        osvReframeEquirectPixel(&p, &pano.src, pano.rgba.data(), vx + 10, vy + 10, px);
        REQUIRE(px[3] == 0.0f);
        // A zero focal is rejected outright (no division by zero, no NaN).
        p.focalPx = 0.0f;
        osvReframeEquirectPixel(&p, &pano.src, pano.rgba.data(), vx + 10, vy + 10, px);
        REQUIRE(px[3] == 0.0f);
        REQUIRE(px[0] == 0.0f);
    }

    SECTION("garbage inputs never crash and produce transparent black") {
        const OsvReframeParams p = reframeParams(cam, outW, outH, vx, vy, true);
        float px[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        osvReframeEquirectPixel(nullptr, &pano.src, pano.rgba.data(), 100, 100, px);
        REQUIRE(px[3] == 0.0f);
        px[3] = 1.0f;
        osvReframeEquirectPixel(&p, nullptr, pano.rgba.data(), 100, 100, px);
        REQUIRE(px[3] == 0.0f);
        px[3] = 1.0f;
        osvReframeEquirectPixel(&p, &pano.src, nullptr, 100, 100, px);
        REQUIRE(px[3] == 0.0f);
        px[3] = 1.0f;
        osvReframeEquirectPixel(&p, &pano.src, pano.rgba.data(), -1, 100, px);
        REQUIRE(px[3] == 0.0f);
        px[3] = 1.0f;
        osvReframeEquirectPixel(&p, &pano.src, pano.rgba.data(), outW, 100, px);
        REQUIRE(px[3] == 0.0f);
        OsvRgbaSource broken = pano.src;
        broken.pitchBytes = 0;
        px[3] = 1.0f;
        osvReframeEquirectPixel(&p, &broken, pano.rgba.data(), 100, 100, px);
        REQUIRE(px[3] == 0.0f);
        OsvReframeParams empty = p;
        empty.viewW = 0;
        px[3] = 1.0f;
        osvReframeEquirectPixel(&empty, &pano.src, pano.rgba.data(), 100, 100, px);
        REQUIRE(px[3] == 0.0f);
        // A null output pointer is simply ignored.
        osvReframeEquirectPixel(&p, &pano.src, pano.rgba.data(), 100, 100, nullptr);
    }
}

// -----------------------------------------------------------------------------
//  CUDA: the device build of the shared function agrees with the host build
// -----------------------------------------------------------------------------
#if defined(OSV_HAVE_CUDA)
TEST_CASE("CUDA equirect reframe matches the CPU function", "[render][reframe][cuda]") {
    std::string reason;
    if (!render::CudaRenderer::available(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    const LabelledEquirect pano = makeLabelled(1440, 720);
    const HalfEquirect halfBgra = toHalf(pano, true);

    VirtualCamera cam;
    cam.projection = Projection::EyeOffset;
    cam.eyeOffset = 0.5;
    cam.w = 1600;
    cam.h = 900;
    cam.hfovDeg = 140.0;
    cam.yawDeg = 160.0;  // across the seam
    cam.pitchDeg = -25.0;
    cam.rollDeg = 8.0;
    const int outW = 1920, outH = 1080;
    const OsvReframeParams p = reframeParams(cam, outW, outH, 160, 90, true);

    struct Source {
        const char* label;
        const OsvRgbaSource* src;
        const void* pixels;
    };
    const Source sources[] = {{"32f RGBA", &pano.src, pano.rgba.data()},
                              {"16f BGRA", &halfBgra.src, halfBgra.data.data()}};
    for (const Source& s : sources) {
        const render::ImageRGBAf cpu = reframeCpu(p, *s.src, s.pixels);
        auto gpu = render::cudaReframeEquirect(0, p, *s.src, s.pixels);
        REQUIRE(gpu.ok());
        const render::ImageDiffStats stats = render::compareImages16(cpu, gpu.value());
        INFO(s.label << ": PSNR " << stats.psnrDb << " dB, max diff " << stats.maxAbsCode << " codes, within2 "
                     << stats.fractionWithin2);
        REQUIRE(stats.psnrDb >= 60.0);
        REQUIRE(stats.fractionWithin2 >= 0.9995);
        REQUIRE(stats.alphaIdentical);
        // Sanity: the letterbox is present on both.
        REQUIRE(gpu.value().pixel(10, 10)[3] == 0.0f);
        REQUIRE(gpu.value().pixel(960, 540)[3] == 1.0f);
    }

    // Defensive host wrapper.
    REQUIRE_FALSE(render::cudaReframeEquirect(0, p, pano.src, nullptr).ok());
    OsvRgbaSource broken = pano.src;
    broken.pitchBytes = 4;
    REQUIRE_FALSE(render::cudaReframeEquirect(0, p, broken, pano.rgba.data()).ok());
    REQUIRE_FALSE(render::cudaReframeEquirect(99, p, pano.src, pano.rgba.data()).ok());
}
#endif
