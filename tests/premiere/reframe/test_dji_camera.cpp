// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_dji_camera.cpp - Open 360 Reframe's DJI lens ("Lens: DJI").
//
// DJI's reframe tools describe the camera with three numbers - Zoom, FOV and
// Correction Angle - that the effect's original (Classic) lens could not
// take: typing them in gave a different picture.  The DJI lens renders
// exactly DJI's camera (docs/research/DJI_CAMERA.md).  What is pinned here:
//
//   1. buildView() on DJI's lens: the projection, the eye distance, a
//      vertical FOV across the requested picture's height, the cover-fit;
//   2. LEGACY: a Classic render - every project saved before the DJI block
//      existed - is bit for bit independent of the new controls;
//   3. the two DJI Studio field observations, rendered through the effect's
//      CPU path and decoded from a labelled panorama;
//   4. the Zoom control's inverse (DJI Studio's zoom path) and the
//      Classic <-> DJI conversions that make switching seamless;
//   5. the supervised behaviour, through the LOADED module - [WP-LENSUI]
//      now driven by the Lens popup, with the old Camera Model checkbox as
//      its hidden mirror;
//   6. the host-parameter plumbing: popups that arrive 0-based on the GPU
//      side, and the matcher with the appended block;
//   7. CPU / GPU parity of the DJI lens through the GPU filter (>= 60 dB);
//   8. [WP-LENSUI] the Lens popup end to end: switching keeps the framing,
//      an old project opens on DJI, and the GPU filter reads the popup (not
//      the checkbox) whether the host numbers it from 0 or from 1.

#include "GpuTestSupport.h"
#include "ReframeTestSupport.h"

#include "ReframeCpu.h"
#include "ReframeParams.h"

#include "MockHost.h"

#include "osv/geom/VirtualCamera.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace osv::reframe;
using namespace osv::reframe::test;
using Catch::Approx;
using osv::premiere::mock::MockHost;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kAspect169 = 16.0 / 9.0;

// ---------------------------------------------------------------------------
//  Settings
// ---------------------------------------------------------------------------

/// A Settings block on DJI's lens (every other control at its default).
[[nodiscard]] Settings djiSettings(double fov, double correction, double pan = 0.0, double tilt = 0.0,
                                   double roll = 0.0, Resolution res = Resolution::MatchSequence) {
    Settings s;
    s.resolution = res;
    s.preset = Preset::Custom;
    s.cameraModel = CameraModel::Dji;
    s.djiFovDeg = fov;
    s.correction = correction;
    s.panDeg = pan;
    s.tiltDeg = tilt;
    s.rollDeg = roll;
    return s;
}

/// A Settings block on the Classic lens.
[[nodiscard]] Settings classicSettings(double fov, double distortion, double pan = 0.0, double tilt = 0.0,
                                       double roll = 0.0, Resolution res = Resolution::MatchSequence) {
    Settings s;
    s.resolution = res;
    s.preset = Preset::Custom;
    s.cameraModel = CameraModel::Classic;
    s.fovDeg = fov;
    s.distortion = distortion;
    s.panDeg = pan;
    s.tiltDeg = tilt;
    s.rollDeg = roll;
    return s;
}

// ---------------------------------------------------------------------------
//  Rays and renders
// ---------------------------------------------------------------------------

/// The body-frame direction a built view sends through pixel (px, py) - the
/// renderer's own chain (osvViewRay, then Rout).  False when the pixel has
/// no ray.
[[nodiscard]] bool viewDirection(const OsvReframeParams& p, double px, double py, double out[3]) {
    const float W = static_cast<float>(p.viewW);
    const float H = static_cast<float>(p.viewH);
    const float nx = static_cast<float>((px + 0.5) - 0.5 * p.viewW);
    const float ny = static_cast<float>(0.5 * p.viewH - (py + 0.5));
    float d[3] = {0.0f, 0.0f, 0.0f};
    if (!osvViewRay(p.projection, p.focalPx, p.eyeOffset, p.tanHalfH, p.tanHalfV, W, H, nx, ny, d)) {
        return false;
    }
    for (int r = 0; r < 3; ++r) {
        out[r] = static_cast<double>(p.Rout[r * 3 + 0]) * d[0] + static_cast<double>(p.Rout[r * 3 + 1]) * d[1] +
                 static_cast<double>(p.Rout[r * 3 + 2]) * d[2];
    }
    return true;
}

/// Angle (rad) between two directions, robust at small angles.
[[nodiscard]] double angleBetween(const double a[3], const double b[3]) {
    const double cx = a[1] * b[2] - a[2] * b[1];
    const double cy = a[2] * b[0] - a[0] * b[2];
    const double cz = a[0] * b[1] - a[1] * b[0];
    const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), dot);
}

/// Largest angle (rad) between the rays two views cast over a pixel grid;
/// negative when one view has a ray where the other has none.
[[nodiscard]] double worstRayDifference(const OsvReframeParams& a, const OsvReframeParams& b) {
    double worst = 0.0;
    for (int py = 0; py < a.viewH; py += std::max(1, a.viewH / 24)) {
        for (int px = 0; px < a.viewW; px += std::max(1, a.viewW / 32)) {
            double da[3];
            double db[3];
            const bool ha = viewDirection(a, px, py, da);
            const bool hb = viewDirection(b, px, py, db);
            if (ha != hb) {
                return -1.0;
            }
            if (ha) {
                worst = std::max(worst, angleBetween(da, db));
            }
        }
    }
    return worst;
}

/// The labelled panorama the renders sample.
[[nodiscard]] const Panorama& panorama() {
    static const Panorama p = makePanorama(2048, 1024);
    return p;
}

/// A CPU render of `s` into a w x h Bgra32f frame from the labelled panorama.
[[nodiscard]] std::vector<std::uint8_t> renderSettings(const Settings& s, int w, int h,
                                                       const Panorama& pano = panorama()) {
    const std::vector<std::uint8_t> srcBytes = packBgra32f(pano, pano.width * 16);
    ConstFrameView src;
    src.base = srcBytes.data();
    src.rowBytes = pano.width * 16;
    src.width = pano.width;
    src.height = pano.height;
    src.layout = PixelLayout::Bgra32f;
    src.topDown = true;
    const KernelSetup setup = buildParams(s, src, w, h, SizePx{});
    REQUIRE(setup.valid);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 16u, 0u);
    FrameView dst;
    dst.base = out.data();
    dst.rowBytes = w * 16;
    dst.width = w;
    dst.height = h;
    dst.layout = PixelLayout::Bgra32f;
    dst.topDown = true;
    REQUIRE(renderCpu(setup, src, dst, nullptr));
    return out;
}

/// The same render as flat RGBA floats (row 0 = top), for PSNR comparisons.
[[nodiscard]] std::vector<float> renderSettingsRgba(const Settings& s, int w, int h) {
    const std::vector<std::uint8_t> bytes = renderSettings(s, w, h);
    std::vector<float> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            readPixelBgra32f(bytes.data(), w * 16, x, y,
                             rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x) * 4u);
        }
    }
    return rgba;
}

/// Longitude / latitude (deg) of a unit body direction in the Standard
/// layout the kernel samples (lon = atan2(x, y), lat = asin(z)).
void lonLatOf(const double d[3], double& lonDeg, double& latDeg) {
    lonDeg = std::atan2(d[0], d[1]) * 180.0 / kPi;
    latDeg = std::asin(std::clamp(d[2], -1.0, 1.0)) * 180.0 / kPi;
}

/// Shortest difference between two longitudes, degrees.
[[nodiscard]] double lonDiff(double a, double b) {
    double d = std::fmod(a - b + 540.0, 360.0);
    if (d < 0.0) {
        d += 360.0;
    }
    return std::fabs(d - 180.0);
}

// ---------------------------------------------------------------------------
//  The loaded module
// ---------------------------------------------------------------------------

/// A mock host with one effect instance whose parameters exist.
struct ModuleFixture {
    MockHost host;
    PF_ProgPtr ref = nullptr;
    PF_InData in{};
    PF_OutData out{};
    std::vector<PF_ParamDef> registered;

    ModuleFixture() {
        ref = host.createEffectRef(0x3000, 21);
        REQUIRE(ref != nullptr);
        in = host.makeInData(ref, {});
        out = host.makeOutData();
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
                PF_Err_NONE);
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_PARAMS_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
                PF_Err_NONE);
        registered = host.addedParams(ref);
        REQUIRE(registered.size() == static_cast<std::size_t>(kParamCount));
        in = host.makeInData(ref, {});
    }
    ~ModuleFixture() {
        if (ref) {
            host.destroyEffectRef(ref);
        }
    }
    ModuleFixture(const ModuleFixture&) = delete;
    ModuleFixture& operator=(const ModuleFixture&) = delete;
};

/// A params array the way PF_Cmd_USER_CHANGED_PARAM receives it (index 0 =
/// input layer), every change flag cleared.  Output Resolution is set to a
/// FIXED 16:9 size so the frame shape the DJI numbers are computed for is
/// exactly 16:9 whatever the mock host knows about the sequence.
struct Params {
    std::vector<PF_ParamDef> storage;
    std::vector<PF_ParamDef*> pointers;

    explicit Params(const std::vector<PF_ParamDef>& registered) {
        storage.resize(registered.size() + 1u);
        std::memset(&storage[0], 0, sizeof(PF_ParamDef));
        storage[0].param_type = PF_Param_LAYER;
        for (std::size_t i = 0; i < registered.size(); ++i) {
            storage[i + 1u] = registered[i];
            storage[i + 1u].uu.change_flags = PF_ChangeFlag_NONE;
        }
        pointers.resize(storage.size());
        for (std::size_t i = 0; i < storage.size(); ++i) {
            pointers[i] = &storage[i];
        }
        at(kIndexOutputResolution).u.pd.value = static_cast<A_long>(Resolution::Fhd1920x1080);
    }

    [[nodiscard]] PF_ParamDef** data() noexcept { return pointers.data(); }
    [[nodiscard]] PF_ParamDef& at(int aeIndex) noexcept { return storage[static_cast<std::size_t>(aeIndex)]; }
    [[nodiscard]] double slider(int aeIndex) noexcept { return static_cast<double>(at(aeIndex).u.fs_d.value); }
    /// [WP-LENSUI] The lens on screen: the Lens popup.
    [[nodiscard]] bool dji() noexcept { return cameraModelFromLensPopup(at(kIndexLens).u.pd.value) == CameraModel::Dji; }
    /// The hidden Camera Model mirror (ticked = DJI).
    [[nodiscard]] bool mirrorDji() noexcept { return at(kIndexCameraModel).u.bd.value != 0; }
    /// Put the instance on a lens the way a host holds it after the effect
    /// supervised the switch: the popup AND its mirror.
    void setLens(CameraModel model) {
        at(kIndexLens).u.pd.value = static_cast<A_long>(lensPopupValue(model));
        at(kIndexCameraModel).u.bd.value = (model == CameraModel::Dji) ? 1 : 0;
    }
    [[nodiscard]] bool marked(int aeIndex) noexcept {
        return (at(aeIndex).uu.change_flags & PF_ChangeFlag_CHANGED_VALUE) != 0;
    }
    void clearFlags() {
        for (PF_ParamDef& d : storage) {
            d.uu.change_flags = PF_ChangeFlag_NONE;
        }
    }
};

/// Run USER_CHANGED_PARAM for `changedIndex` through the loaded module.
void userChanged(ModuleFixture& f, Params& p, int changedIndex) {
    PF_UserChangedParamExtra extra{};
    extra.param_index = changedIndex;
    REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_USER_CHANGED_PARAM, &f.in, &f.out, p.data(), nullptr,
                                                  &extra) == PF_Err_NONE);
}

}  // namespace

// ===========================================================================
//  1. buildView on DJI's lens
// ===========================================================================

TEST_CASE("buildView on DJI's lens: its projection, its eye distance, a vertical FOV", "[reframe][geometry][dji]") {
    SECTION("the frame's own shape: the FOV spans the frame height exactly") {
        const ViewSetup v = buildView(djiSettings(103.3, 0.67), 1920, 1080, SizePx{});
        REQUIRE(v.valid);
        CHECK(v.params.projection == OSV_PROJ_DJI_SPHERE);
        CHECK(v.params.eyeOffset == 0.67f);
        const double focal = 540.0 / std::tan(0.5 * 103.3 * kPi / 180.0);
        CHECK(v.params.focalPx == static_cast<float>(focal));
        CHECK(v.params.tanHalfV == Approx(540.0 / focal).epsilon(1e-6));
        CHECK(v.params.tanHalfH == Approx(960.0 / focal).epsilon(1e-6));
        // The library's reference camera agrees on the focal length.
        osv::geom::DjiSphereCamera ref;
        ref.w = 1920;
        ref.h = 1080;
        ref.vfovDeg = 103.3;
        ref.eyeDistance = 0.67;
        CHECK(v.params.focalPx == Approx(ref.focalPx()).epsilon(1e-6));
        // A preview-scaled frame of the same shape is the same camera, scaled.
        const ViewSetup half = buildView(djiSettings(103.3, 0.67), 960, 540, SizePx{1920, 1080});
        REQUIRE(half.valid);
        CHECK(half.params.focalPx == Approx(0.5 * focal).epsilon(1e-6));
    }
    SECTION("a requested picture WIDER than the frame keeps the FOV on the height and crops the sides") {
        // 16:9 into 4:3, and 16:9 into a portrait frame: the picture is
        // scaled until its height is the frame's, so the vertical FOV spans
        // the frame height exactly and the sides are cropped.
        const ViewSetup v = buildView(djiSettings(90.0, 0.5, 0, 0, 0, Resolution::Fhd1920x1080), 1440, 1080, SizePx{});
        REQUIRE(v.valid);
        CHECK(v.params.focalPx == Approx(540.0 / std::tan(0.25 * kPi)).epsilon(1e-6));
        const ViewSetup portrait =
            buildView(djiSettings(90.0, 0.5, 0, 0, 0, Resolution::Fhd1920x1080), 1080, 1920, SizePx{});
        REQUIRE(portrait.valid);
        CHECK(portrait.params.focalPx == Approx(960.0 / std::tan(0.25 * kPi)).epsilon(1e-6));
    }
    SECTION("a requested picture TALLER than the frame spans the frame width and crops top and bottom") {
        // 16:9 requested into a 2560 x 720 (32:9) frame: the picture is
        // scaled until its WIDTH is the frame's, so its height in frame
        // pixels is 2560 * 1080 / 1920 = 1440 and the FOV spans that.
        const ViewSetup v = buildView(djiSettings(90.0, 0.5, 0, 0, 0, Resolution::Fhd1920x1080), 2560, 720, SizePx{});
        REQUIRE(v.valid);
        CHECK(v.params.focalPx == Approx(0.5 * 2560.0 * 1080.0 / 1920.0 / std::tan(0.25 * kPi)).epsilon(1e-6));
    }
    SECTION("hostile values are clamped to the controls' valid ranges, never rendered raw") {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const ViewSetup a = buildView(djiSettings(nan, nan), 1920, 1080, SizePx{});
        REQUIRE(a.valid);
        CHECK(a.params.eyeOffset == static_cast<float>(OSV_REFRAME_CORRECTION_DEFAULT));
        CHECK(a.params.focalPx ==
              Approx(540.0 / std::tan(0.5 * OSV_REFRAME_DJI_FOV_DEFAULT * kPi / 180.0)).epsilon(1e-6));
        const ViewSetup b = buildView(djiSettings(500.0, 99.0), 1920, 1080, SizePx{});
        REQUIRE(b.valid);
        CHECK(b.params.eyeOffset == static_cast<float>(OSV_REFRAME_CORRECTION_VALID_MAX));
        CHECK(b.params.focalPx ==
              Approx(540.0 / std::tan(0.5 * OSV_REFRAME_DJI_FOV_VALID_MAX * kPi / 180.0)).epsilon(1e-5));
        const ViewSetup c = buildView(djiSettings(-5.0, -1.0), 1920, 1080, SizePx{});
        REQUIRE(c.valid);
        CHECK(c.params.eyeOffset == 0.0f);
        CHECK(std::isfinite(c.params.focalPx));
        // And the frame checks still apply first.
        CHECK_FALSE(buildView(djiSettings(60.0, 0.6), 0, 1080, SizePx{}).valid);
    }
}

// ===========================================================================
//  2. Legacy: a Classic render never depends on the new controls
// ===========================================================================

TEST_CASE("a Classic render is bit for bit independent of the DJI controls", "[reframe][geometry][dji][legacy]") {
    // Every project saved before the DJI block existed loads it at its
    // defaults - Classic - and must render exactly as it did.  So on the
    // Classic lens the DJI controls must not touch a single bit, whatever
    // they hold.
    struct ClassicCase {
        double fov;
        double distortion;
        double pan;
        double tilt;
        double roll;
        Resolution res;
        int w;
        int h;
    };
    const ClassicCase cases[] = {
        {120.0, 15.0, 0.0, 0.0, 0.0, Resolution::MatchSequence, 1920, 1080},   // the defaults
        {150.0, 40.0, -60.0, -5.0, 0.0, Resolution::Fhd1920x1080, 1920, 1080}, // Ultra Wide
        {300.0, 100.0, 10.0, -90.0, 0.0, Resolution::Uhd3840x2160, 1280, 720}, // Asteroid
        {90.0, 0.0, 30.0, 10.0, 5.0, Resolution::Fhd1920x1080, 1080, 1920},    // cover-fit crop
        {210.0, 0.0, 0.0, 0.0, 0.0, Resolution::MatchSequence, 1920, 1080},    // the auto ramp
    };
    for (const ClassicCase& c : cases) {
        INFO("classic fov " << c.fov << " distortion " << c.distortion);
        Settings plain = classicSettings(c.fov, c.distortion, c.pan, c.tilt, c.roll, c.res);
        const ViewSetup reference = buildView(plain, c.w, c.h, SizePx{});
        REQUIRE(reference.valid);
        CHECK(reference.params.projection == OSV_PROJ_EYE_OFFSET);

        // The DJI controls holding anything at all - including garbage.
        for (const double junk : {0.0, 0.37, 1.8, 179.0, -1e9, std::numeric_limits<double>::quiet_NaN()}) {
            Settings noisy = plain;
            noisy.djiFovDeg = junk;
            noisy.correction = junk;
            noisy.zoomDeg = junk;
            noisy.dragSensitivity = junk;
            const ViewSetup v = buildView(noisy, c.w, c.h, SizePx{});
            REQUIRE(v.valid);
            CHECK(std::memcmp(&v.params, &reference.params, sizeof(OsvReframeParams)) == 0);
        }
        // A default-constructed Settings is Classic: that IS an old project.
        Settings defaults;
        defaults.resolution = c.res;
        defaults.fovDeg = c.fov;
        defaults.distortion = c.distortion;
        defaults.panDeg = c.pan;
        defaults.tiltDeg = c.tilt;
        defaults.rollDeg = c.roll;
        defaults.preset = Preset::Custom;
        const ViewSetup d = buildView(defaults, c.w, c.h, SizePx{});
        REQUIRE(d.valid);
        CHECK(std::memcmp(&d.params, &reference.params, sizeof(OsvReframeParams)) == 0);
    }

    // And the Classic numbers themselves are the ones the eye-offset camera
    // always produced: FOV 120 / Distortion 15 on 1920 wide is the focal
    // (W/2)(d + cos h)/((1 + d) sin h) with h = 60 deg, d = 0.15.
    const ViewSetup def = buildView(classicSettings(120.0, 15.0), 1920, 1080, SizePx{});
    REQUIRE(def.valid);
    const double h = 60.0 * kPi / 180.0;
    CHECK(def.params.focalPx == Approx(960.0 * (0.15 + std::cos(h)) / (1.15 * std::sin(h))).epsilon(1e-6));
    CHECK(def.params.eyeOffset == 0.15f);
}

// ===========================================================================
//  3. The DJI Studio field observations, rendered
// ===========================================================================

TEST_CASE("the two DJI Studio settings render the framing DJI's camera predicts", "[reframe][cpu][dji]") {
    // The same clip in DJI Studio: Zoom 207.1 / FOV 103.3 / Correction 0.67
    // ("a gentle wide fisheye look") and Zoom 201.7 / FOV 150 / Correction
    // 0.34 ("an extreme radial tunnel stretch").  Rendered here on DJI's lens
    // and decoded from a labelled panorama, every probed pixel must look
    // where DJI's camera (the library's double-precision reference) says.
    struct Observation {
        const char* name;
        double fov;
        double correction;
    };
    const Observation seen[] = {{"gentle fisheye 103.3 / 0.67", 103.3, 0.67}, {"tunnel 150 / 0.34", 150.0, 0.34}};
    constexpr int kW = 960;
    constexpr int kH = 540;
    for (const Observation& o : seen) {
        INFO(o.name);
        const std::vector<std::uint8_t> frame = renderSettings(djiSettings(o.fov, o.correction), kW, kH);
        osv::geom::DjiSphereCamera ref;
        ref.w = kW;
        ref.h = kH;
        ref.vfovDeg = o.fov;
        ref.eyeDistance = o.correction;
        // The centre, the edge midpoints, a corner and two interior points.
        const int probes[][2] = {{480, 270}, {kW - 1, 270}, {0, 270}, {480, 0}, {480, kH - 1},
                                 {kW - 1, 0}, {700, 400}, {200, 120}};
        for (const auto& pr : probes) {
            INFO("pixel (" << pr[0] << ", " << pr[1] << ")");
            osv::Vec3d d;
            REQUIRE(ref.pixelToRay(pr[0], pr[1], d));
            const double dir[3] = {d.x, d.y, d.z};
            double lon = 0.0;
            double lat = 0.0;
            lonLatOf(dir, lon, lat);
            float rgba[4];
            readPixelBgra32f(frame.data(), kW * 16, pr[0], pr[1], rgba);
            CHECK(rgba[3] == Approx(1.0f));
            CHECK(Panorama::decodeLatitude(rgba) == Approx(lat).margin(0.25));
            // Longitude is meaningless at the poles; everywhere else it must
            // match too.
            if (std::fabs(lat) < 80.0) {
                CHECK(lonDiff(Panorama::decodeLongitude(rgba), lon) < 0.25);
            }
        }
        // DJI's Zoom is the angle the centre row's two edges span: the edge
        // pixels look half of it off axis (to within their half pixel).
        osv::Vec3d edge;
        REQUIRE(ref.pixelToRay(kW - 1, 270, edge));
        const double offAxis = std::acos(std::clamp(edge.y, -1.0, 1.0)) * 180.0 / kPi;
        CHECK(2.0 * offAxis == Approx(djiZoomDeg(DjiLens{o.fov, o.correction}, kAspect169)).margin(0.5));
    }
}

// ===========================================================================
//  4. Zoom's inverse and the conversions
// ===========================================================================

TEST_CASE("djiZoomTo walks DJI Studio's zoom path to the requested Zoom", "[reframe][dji]") {
    // From DJI's Wide (60 / 0.6) to the first screenshot's Zoom.
    const DjiLens to = djiZoomTo(207.1, DjiLens{60.0, 0.6}, kAspect169);
    CHECK(djiZoomDeg(to, kAspect169) == Approx(207.1).margin(1e-6));
    // On the path: fov - 60 = 130 (correction - 0.6).
    CHECK(to.fovDeg - 60.0 == Approx(130.0 * (to.correction - 0.6)).margin(1e-6));
    CHECK(to.fovDeg == Approx(87.1608).margin(1e-3));
    CHECK(to.correction == Approx(0.80893).margin(1e-4));

    // Past either end the path stops at DJI Studio's limits.
    const DjiLens wideOpen = djiZoomTo(359.0, DjiLens{60.0, 0.6}, kAspect169);
    CHECK(wideOpen.fovDeg == Approx(150.0));
    CHECK(wideOpen.correction == Approx(1.0));
    const DjiLens shut = djiZoomTo(1.0, DjiLens{60.0, 0.6}, kAspect169);
    CHECK(shut.fovDeg == Approx(20.0));
    CHECK(shut.correction == Approx(0.0));

    // A Crystal Ball (eye 1.8, beyond Studio's 1.0) keeps its range: zooming
    // it in moves the eye down from 1.8, not from a snapped 1.0.
    const DjiLens crystal{75.0, 1.8};
    const double z0 = djiZoomDeg(crystal, kAspect169);
    const DjiLens nudged = djiZoomTo(z0 - 5.0, crystal, kAspect169);
    CHECK(nudged.correction < 1.8);
    CHECK(nudged.correction > 1.0);

    // Garbage in leaves the lens alone (sanitised).
    const DjiLens same = djiZoomTo(std::numeric_limits<double>::quiet_NaN(), DjiLens{60.0, 0.6}, kAspect169);
    CHECK(same.fovDeg == 60.0);
    CHECK(same.correction == 0.6);
}

TEST_CASE("DJI Studio's Manual Framing read-outs lie on the zoom path the effect walks", "[reframe][dji]") {
    // Four read-outs of DJI Studio's Manual Framing panel on a 16:9 canvas,
    // all on the path out of the zoomed-in corner (FOV 20 / Correction 0,
    // where both of DJI Studio's limits meet).  Each is Zoom, FOV and
    // Correction Angle as DJI Studio displays them: Zoom and FOV to a tenth
    // of a degree, Correction to a hundredth.
    struct ReadOut {
        double zoom;
        double fov;
        double correction;
    };
    const ReadOut readOuts[] = {
        {45.5, 25.4, 0.04},
        {101.4, 52.5, 0.25},
        {221.5, 112.1, 0.71},
        {256.6, 128.3, 0.83},
    };
    const DjiLens zoomedIn{OSV_REFRAME_DJI_STUDIO_FOV_MIN, OSV_REFRAME_CORRECTION_VALID_MIN};
    for (const ReadOut& r : readOuts) {
        INFO("DJI Studio shows Zoom " << r.zoom << ", FOV " << r.fov << ", Correction " << r.correction);
        // The path from the zoomed-in corner is FOV = 20 + 130 x Correction:
        // every read-out sits on it, to the precision DJI displays.
        CHECK(r.fov == Approx(OSV_REFRAME_DJI_STUDIO_FOV_MIN + OSV_REFRAME_DJI_ZOOM_FOV_PER_CORRECTION * r.correction)
                           .margin(0.5));
        // DJI's own Zoom formula gives the Zoom DJI shows for the FOV and
        // Correction it shows (the displayed values are rounded, so the
        // margin covers half a unit of the last digit of both).
        CHECK(djiZoomDeg(DjiLens{r.fov, r.correction}, kAspect169) == Approx(r.zoom).margin(1.2));
        // And walking the effect's zoom path (the Zoom control, the overlay's
        // Ctrl + drag) to that Zoom from the same corner lands on DJI's numbers.
        const DjiLens walked = djiZoomTo(r.zoom, zoomedIn, kAspect169);
        CHECK(djiZoomDeg(walked, kAspect169) == Approx(r.zoom).margin(1e-6));
        CHECK(walked.fovDeg == Approx(r.fov).margin(0.5));
        CHECK(walked.correction == Approx(r.correction).margin(0.006));
    }
}

TEST_CASE("switching lenses carries the picture across", "[reframe][dji]") {
    SECTION("Classic -> DJI frames exactly what Classic framed") {
        // Including the automatic ramp (210 deg) and the Classic presets.
        const ClassicLens classics[] = {{120.0, 15.0}, {150.0, 40.0}, {95.0, 0.0}, {210.0, 0.0}, {300.0, 100.0}};
        for (const ClassicLens& c : classics) {
            INFO("classic " << c.fovDeg << " / " << c.distortion);
            const DjiLens lens = djiFromClassic(c, kAspect169);
            const ViewSetup a = buildView(classicSettings(c.fovDeg, c.distortion, 25, -10, 3), 1920, 1080, SizePx{});
            const ViewSetup b = buildView(djiSettings(lens.fovDeg, lens.correction, 25, -10, 3), 1920, 1080, SizePx{});
            REQUIRE(a.valid);
            REQUIRE(b.valid);
            const double worst = worstRayDifference(a.params, b.params);
            INFO("worst ray difference " << worst << " rad");
            CHECK(worst >= 0.0);
            CHECK(worst < 2e-4);
            // And DJI's Zoom of the converted lens is the Classic FOV.
            CHECK(djiZoomDeg(lens, kAspect169) == Approx(c.fovDeg).margin(1e-6));
        }
    }
    SECTION("DJI -> Classic is exact whenever Classic can express the lens") {
        const DjiLens lens{60.0, 0.6};  // DJI's Wide: no ramp above 0.6 at its Zoom
        const ClassicLens c = classicFromDji(lens, kAspect169);
        CHECK(c.fovDeg == Approx(djiZoomDeg(lens, kAspect169)));
        CHECK(c.distortion == Approx(60.0));
        const ViewSetup a = buildView(djiSettings(60.0, 0.6), 1920, 1080, SizePx{});
        const ViewSetup b = buildView(classicSettings(c.fovDeg, c.distortion), 1920, 1080, SizePx{});
        REQUIRE(a.valid);
        REQUIRE(b.valid);
        const double worst = worstRayDifference(a.params, b.params);
        CHECK(worst >= 0.0);
        CHECK(worst < 2e-4);
        // A Crystal Ball has no Classic equivalent; the nearest is clamped.
        const ClassicLens crystal = classicFromDji(DjiLens{75.0, 1.8}, kAspect169);
        CHECK(crystal.distortion == Approx(100.0));
        CHECK(crystal.fovDeg <= OSV_REFRAME_FOV_VALID_MAX);
    }
    SECTION("framingAspect names the shape the numbers are computed for") {
        CHECK(framingAspect(Resolution::Fhd1920x1080, SizePx{}) == Approx(kAspect169));
        CHECK(framingAspect(Resolution::MatchSequence, SizePx{1080, 1920}) == Approx(0.5625));
        CHECK(framingAspect(Resolution::MatchSequence, SizePx{}) == Approx(kAspect169));
    }
}

// ===========================================================================
//  5. The supervised behaviour, through the loaded module
// ===========================================================================

TEST_CASE("a preset writes DJI's numbers and switches to DJI's lens", "[reframe][supervise][dji]") {
    ModuleFixture f;
    for (const PresetEntry& entry : kPresetTable) {
        if (!entry.writesControls) {
            continue;
        }
        INFO("preset '" << entry.label << "'");
        Params p(f.registered);
        REQUIRE(p.dji());                 // [WP-LENSUI] a fresh instance is on DJI's lens...
        p.setLens(CameraModel::Classic);  // ...so start from Classic to see the switch
        p.at(kIndexPreset).u.pd.value = static_cast<A_long>(entry.value);
        userChanged(f, p, kIndexPreset);

        // DJI's landscape column (the params ask for 1920 x 1080).
        CHECK(p.slider(kIndexDjiFov) == Approx(entry.djiFovLandscapeDeg));
        CHECK(p.slider(kIndexCorrection) == Approx(entry.correction));
        CHECK(p.slider(kIndexZoom) ==
              Approx(djiZoomDeg(DjiLens{entry.djiFovLandscapeDeg, entry.correction}, kAspect169)).margin(1e-3));
        CHECK(p.dji());
        CHECK(p.marked(kIndexLens));
        // The hidden mirror follows the popup.
        CHECK(p.mirrorDji());
        CHECK(p.marked(kIndexCameraModel));
        CHECK(p.marked(kIndexDjiFov));
        CHECK(p.marked(kIndexCorrection));
        CHECK(p.marked(kIndexZoom));
        // The Classic numbers are still written (the nearest Classic look
        // should the user switch back), and so is the tilt.
        CHECK(p.slider(kIndexFov) == Approx(entry.fovDeg));
        CHECK(p.slider(kIndexDistortion) == Approx(entry.distortion));
        CHECK(static_cast<double>(p.at(kIndexTilt).u.ad.value) / 65536.0 == entry.tiltDeg);
        // The preset keeps the value the user chose.
        CHECK(p.at(kIndexPreset).u.pd.value == static_cast<A_long>(entry.value));
    }
}

TEST_CASE("editing a DJI control switches to DJI's lens without a jump", "[reframe][supervise][dji]") {
    ModuleFixture f;

    SECTION("DJI FOV edited on a Classic instance keeps the Classic eye distance") {
        Params p(f.registered);
        p.setLens(CameraModel::Classic);
        p.at(kIndexPreset).u.pd.value = static_cast<A_long>(Preset::Wide);
        p.at(kIndexFov).u.fs_d.value = 150.0f;
        p.at(kIndexDistortion).u.fs_d.value = 40.0f;
        p.at(kIndexDjiFov).u.fs_d.value = 90.0f;  // the user's edit
        userChanged(f, p, kIndexDjiFov);
        CHECK(p.dji());
        CHECK(p.slider(kIndexDjiFov) == Approx(90.0));                       // theirs, untouched
        CHECK_FALSE(p.marked(kIndexDjiFov));
        CHECK(p.slider(kIndexCorrection) == Approx(effectiveEyeOffset(40.0, 150.0)).margin(1e-6));
        CHECK(p.marked(kIndexCorrection));
        CHECK(p.slider(kIndexZoom) ==
              Approx(djiZoomDeg(DjiLens{90.0, p.slider(kIndexCorrection)}, kAspect169)).margin(1e-3));
        CHECK(static_cast<Preset>(p.at(kIndexPreset).u.pd.value) == Preset::Custom);
    }
    SECTION("Correction edited on a Classic instance keeps the Classic pinhole FOV") {
        Params p(f.registered);
        p.setLens(CameraModel::Classic);
        p.at(kIndexFov).u.fs_d.value = 120.0f;
        p.at(kIndexDistortion).u.fs_d.value = 15.0f;
        p.at(kIndexCorrection).u.fs_d.value = 0.5f;  // the user's edit
        userChanged(f, p, kIndexCorrection);
        CHECK(p.dji());
        CHECK(p.slider(kIndexCorrection) == Approx(0.5));
        CHECK(p.slider(kIndexDjiFov) ==
              Approx(djiFromClassic(ClassicLens{120.0, 15.0}, kAspect169).fovDeg).margin(1e-3));
    }
    SECTION("Zoom moves FOV and Correction along DJI Studio's zoom path") {
        Params p(f.registered);
        p.setLens(CameraModel::Dji);
        p.at(kIndexDjiFov).u.fs_d.value = 60.0f;
        p.at(kIndexCorrection).u.fs_d.value = 0.6f;
        p.at(kIndexZoom).u.fs_d.value = 207.1f;  // the user's edit
        userChanged(f, p, kIndexZoom);
        CHECK(p.slider(kIndexDjiFov) == Approx(87.1608).margin(2e-3));
        CHECK(p.slider(kIndexCorrection) == Approx(0.80893).margin(2e-4));
        // Zoom is rewritten with what the lens now shows.
        CHECK(p.slider(kIndexZoom) == Approx(207.1).margin(1e-3));
        CHECK(p.dji());
    }
    SECTION("a Zoom past DJI's limits stops at them and says so") {
        Params p(f.registered);
        p.setLens(CameraModel::Dji);
        p.at(kIndexZoom).u.fs_d.value = 359.0f;
        userChanged(f, p, kIndexZoom);
        CHECK(p.slider(kIndexDjiFov) == Approx(150.0));
        CHECK(p.slider(kIndexCorrection) == Approx(1.0));
        CHECK(p.slider(kIndexZoom) == Approx(djiZoomDeg(DjiLens{150.0, 1.0}, kAspect169)).margin(1e-3));
    }
    SECTION("editing a Classic control goes back to Classic") {
        Params p(f.registered);
        p.setLens(CameraModel::Dji);
        userChanged(f, p, kIndexFov);
        CHECK_FALSE(p.dji());
        CHECK(p.marked(kIndexLens));
        CHECK_FALSE(p.mirrorDji());
        CHECK(p.marked(kIndexCameraModel));
    }
    SECTION("picking DJI in the Lens popup converts Classic's look; picking Classic converts back") {
        Params p(f.registered);
        p.setLens(CameraModel::Classic);
        p.at(kIndexFov).u.fs_d.value = 120.0f;
        p.at(kIndexDistortion).u.fs_d.value = 15.0f;
        p.at(kIndexLens).u.pd.value = static_cast<A_long>(LensPopup::Dji);  // the user picks DJI
        userChanged(f, p, kIndexLens);
        // Classic FOV is the visible angle, so DJI's Zoom must equal it.
        CHECK(p.slider(kIndexZoom) == Approx(120.0).margin(1e-3));
        CHECK(p.marked(kIndexDjiFov));
        CHECK(p.marked(kIndexCorrection));
        // The popup the user set is not re-marked; the mirror follows it.
        CHECK_FALSE(p.marked(kIndexLens));
        CHECK(p.mirrorDji());
        CHECK(p.marked(kIndexCameraModel));
        // The Classic controls, now hidden, keep what they held.
        CHECK_FALSE(p.marked(kIndexFov));
        CHECK_FALSE(p.marked(kIndexDistortion));
        // And the panel is asked to redraw, which brings the
        // UPDATE_PARAMS_UI that shows DJI's controls.
        CHECK((f.out.out_flags & PF_OutFlag_REFRESH_UI) != 0);

        p.clearFlags();
        f.out.out_flags = 0;
        p.at(kIndexDjiFov).u.fs_d.value = 60.0f;
        p.at(kIndexCorrection).u.fs_d.value = 0.6f;
        p.at(kIndexLens).u.pd.value = static_cast<A_long>(LensPopup::Classic);  // and picks Classic
        userChanged(f, p, kIndexLens);
        CHECK(p.slider(kIndexFov) == Approx(djiZoomDeg(DjiLens{60.0, 0.6}, kAspect169)).margin(1e-3));
        CHECK(p.slider(kIndexDistortion) == Approx(60.0).margin(1e-3));
        CHECK_FALSE(p.mirrorDji());
        CHECK_FALSE(p.marked(kIndexDjiFov));
        CHECK((f.out.out_flags & PF_OutFlag_REFRESH_UI) != 0);
    }
    SECTION("ticking the hidden checkbox - a host that shows it - switches the popup the same way") {
        Params p(f.registered);
        p.setLens(CameraModel::Classic);
        p.at(kIndexFov).u.fs_d.value = 120.0f;
        p.at(kIndexDistortion).u.fs_d.value = 15.0f;
        p.at(kIndexCameraModel).u.bd.value = 1;  // the user ticks "DJI"
        userChanged(f, p, kIndexCameraModel);
        CHECK(p.dji());
        CHECK(p.marked(kIndexLens));
        CHECK(p.slider(kIndexZoom) == Approx(120.0).margin(1e-3));
    }
    SECTION("re-picking the lens already on screen converts nothing") {
        Params p(f.registered);
        p.setLens(CameraModel::Dji);
        p.at(kIndexPreset).u.pd.value = static_cast<A_long>(Preset::Wide);
        p.at(kIndexDjiFov).u.fs_d.value = 103.3f;
        p.at(kIndexCorrection).u.fs_d.value = 0.67f;
        p.at(kIndexFov).u.fs_d.value = 150.0f;
        userChanged(f, p, kIndexLens);  // DJI -> DJI
        CHECK(p.slider(kIndexDjiFov) == Approx(103.3).margin(1e-4));
        CHECK(p.slider(kIndexCorrection) == Approx(0.67).margin(1e-6));
        CHECK(static_cast<Preset>(p.at(kIndexPreset).u.pd.value) == Preset::Wide);
        for (int i = 1; i <= kParamCount; ++i) {
            CHECK_FALSE(p.marked(i));
        }
    }
    SECTION("a stale mirror (an old project saved unticked) still leaves Classic beside Custom") {
        // A WP-CAMERA project saved with the checkbox unticked opens on DJI
        // (the popup's default) with the mirror still saying Classic.
        // Picking Classic then looks like a re-pick - nothing to convert, so
        // the project's own Classic numbers come back - but Preset must still
        // become Custom, the pairing the GPU path's popup decoding relies on,
        // and the panel must still be asked to refresh.
        Params p(f.registered);
        p.at(kIndexLens).u.pd.value = static_cast<A_long>(LensPopup::Classic);  // the user picks Classic
        p.at(kIndexCameraModel).u.bd.value = 0;                                 // the stale mirror
        p.at(kIndexPreset).u.pd.value = static_cast<A_long>(Preset::Wide);
        p.at(kIndexFov).u.fs_d.value = 133.0f;
        p.at(kIndexDistortion).u.fs_d.value = 22.0f;
        f.out.out_flags = 0;
        userChanged(f, p, kIndexLens);
        CHECK_FALSE(p.dji());
        CHECK(p.slider(kIndexFov) == Approx(133.0));
        CHECK(p.slider(kIndexDistortion) == Approx(22.0));
        CHECK_FALSE(p.marked(kIndexFov));
        CHECK(static_cast<Preset>(p.at(kIndexPreset).u.pd.value) == Preset::Custom);
        CHECK(p.marked(kIndexPreset));
        CHECK((f.out.out_flags & PF_OutFlag_REFRESH_UI) != 0);
    }
    SECTION("Drag Sensitivity is a preference: editing it changes nothing else") {
        Params p(f.registered);
        p.at(kIndexPreset).u.pd.value = static_cast<A_long>(Preset::Wide);
        p.at(kIndexDragSensitivity).u.fs_d.value = 4.0f;
        userChanged(f, p, kIndexDragSensitivity);
        CHECK(static_cast<Preset>(p.at(kIndexPreset).u.pd.value) == Preset::Wide);
        CHECK(p.dji());  // the default lens, untouched
        for (int i = 1; i <= kParamCount; ++i) {
            CHECK_FALSE(p.marked(i));
        }
    }
}

TEST_CASE("the DJI controls carry DJI's ranges, defaults and flags", "[reframe][params][dji]") {
    ModuleFixture f;
    const std::vector<PF_ParamDef>& r = f.registered;
    // [WP-LENSUI] The Camera Model checkbox is the Lens popup's hidden
    // mirror: registered invisible, ticked (DJI) like the popup's default.
    const PF_ParamDef& model = r[kIndexCameraModel - 1];
    REQUIRE(model.param_type == PF_Param_CHECKBOX);
    CHECK(model.u.bd.dephault == OSV_REFRAME_CAMERA_MODEL_DEFAULT);
    CHECK(cameraModelFromCheckbox(model.u.bd.dephault) == CameraModel::Dji);
    CHECK((model.ui_flags & PF_PUI_INVISIBLE) != 0);
    CHECK((model.flags & PF_ParamFlag_CANNOT_TIME_VARY) != 0);
    CHECK((model.flags & PF_ParamFlag_SUPERVISE) != 0);

    const PF_ParamDef& zoom = r[kIndexZoom - 1];
    REQUIRE(zoom.param_type == PF_Param_FLOAT_SLIDER);
    CHECK((zoom.flags & PF_ParamFlag_CANNOT_TIME_VARY) != 0);  // never rendered from
    CHECK(zoom.u.fs_d.dephault == static_cast<PF_FpShort>(OSV_REFRAME_ZOOM_DEFAULT));
    // The default Zoom is the default lens's Zoom on 16:9, to the slider's tenths.
    CHECK(OSV_REFRAME_ZOOM_DEFAULT ==
          Approx(djiZoomDeg(DjiLens{OSV_REFRAME_DJI_FOV_DEFAULT, OSV_REFRAME_CORRECTION_DEFAULT}, kAspect169))
              .margin(0.05));

    const PF_ParamDef& fov = r[kIndexDjiFov - 1];
    REQUIRE(fov.param_type == PF_Param_FLOAT_SLIDER);
    CHECK(fov.u.fs_d.valid_min == static_cast<PF_FpShort>(1.0));    // DJI's plug-in
    CHECK(fov.u.fs_d.valid_max == static_cast<PF_FpShort>(178.0));
    CHECK(fov.u.fs_d.slider_min == static_cast<PF_FpShort>(20.0));  // DJI Studio
    CHECK(fov.u.fs_d.slider_max == static_cast<PF_FpShort>(150.0));
    CHECK(fov.u.fs_d.dephault == static_cast<PF_FpShort>(60.0));
    CHECK((fov.flags & PF_ParamFlag_CANNOT_TIME_VARY) == 0);        // keyframeable, like DJI's

    const PF_ParamDef& cor = r[kIndexCorrection - 1];
    REQUIRE(cor.param_type == PF_Param_FLOAT_SLIDER);
    CHECK(cor.u.fs_d.valid_max == static_cast<PF_FpShort>(1.8));    // the Crystal Ball
    CHECK(cor.u.fs_d.dephault == static_cast<PF_FpShort>(0.6));
    CHECK(cor.u.fs_d.precision == PF_Precision_HUNDREDTHS);

    const PF_ParamDef& drag = r[kIndexDragSensitivity - 1];
    REQUIRE(drag.param_type == PF_Param_FLOAT_SLIDER);
    CHECK(drag.u.fs_d.dephault == static_cast<PF_FpShort>(2.0));    // the old constant
    CHECK((drag.flags & PF_ParamFlag_SUPERVISE) == 0);
}

// ===========================================================================
//  6. Host parameter plumbing
// ===========================================================================

TEST_CASE("decodeHostPopup learns whether a host counts popup entries from 0 or 1", "[reframe][params][dji]") {
    SECTION("0 can only be a 0-based host") {
        PopupBase base = PopupBase::Unknown;
        CHECK(decodeHostPopup(0, 5, &base) == 1);
        CHECK(base == PopupBase::Zero);
        // ...and from then on every value is shifted, including the
        // ambiguous ones.
        CHECK(decodeHostPopup(3, 6, &base) == 4);  // Premiere 26.2.2's Preset at its default: Wide
        CHECK(base == PopupBase::Zero);
    }
    SECTION("the entry count can only be a 1-based host") {
        PopupBase base = PopupBase::Unknown;
        CHECK(decodeHostPopup(6, 6, &base) == 6);
        CHECK(base == PopupBase::One);
        CHECK(decodeHostPopup(1, 5, &base) == 1);
        CHECK(base == PopupBase::One);
    }
    SECTION("while nothing is known, values read the After Effects way") {
        PopupBase base = PopupBase::Unknown;
        CHECK(decodeHostPopup(2, 5, &base) == 2);
        CHECK(base == PopupBase::Unknown);
        CHECK(decodeHostPopup(3, 5, nullptr) == 3);
        CHECK(decodeHostPopup(0, 5, nullptr) == 1);  // 0 is still unambiguous
    }
}

TEST_CASE("matchHostParams with the appended DJI block", "[reframe][params][dji]") {
    HostParamMap map{};

    SECTION("the whole value list maps one to one, the DJI controls included") {
        std::vector<HostParamKind> all(kValueParamKind, kValueParamKind + kValueParamCount);
        REQUIRE(matchHostParams(all.data(), static_cast<int>(all.size()), &map));
        for (int i = 0; i < kValueParamCount; ++i) {
            CHECK(map[kValueParamAeIndex[i]] == i);
        }
        CHECK(map[kIndexCameraModel] == 11);
        CHECK(map[kIndexDragSensitivity] == 15);
        CHECK(map[kIndexLens] == 16);
        // [WP-EASING] The Keyframe Easing popup, appended after the Lens.
        CHECK(map[kIndexKeyframeEasing] == 17);
    }
    SECTION("a list that stops before the Lens popup maps the rest; the Lens reads its default") {
        // A host whose list predates [WP-LENSUI]: every control up to Drag
        // Sensitivity, then nothing - so neither the Lens popup nor the
        // Keyframe Easing popup appended after it ([WP-EASING]).
        const std::vector<HostParamKind> noLens(kValueParamKind, kValueParamKind + kValueParamCount - 2);
        REQUIRE(matchHostParams(noLens.data(), static_cast<int>(noLens.size()), &map));
        CHECK(map[kIndexDragSensitivity] == 15);
        CHECK(map[kIndexLens] == -1);
        CHECK(map[kIndexKeyframeEasing] == -1);
    }
    SECTION("Premiere's compact layout of the NEW list: the Source group hidden") {
        std::vector<HostParamKind> compact;
        for (int i = 0; i < kValueParamCount; ++i) {
            const int ae = kValueParamAeIndex[i];
            if (ae != kIndexSourcePan && ae != kIndexSourceTilt && ae != kIndexSourceRoll) {
                compact.push_back(kValueParamKind[i]);
            }
        }
        REQUIRE(compact.size() == 15u);
        REQUIRE(matchHostParams(compact.data(), static_cast<int>(compact.size()), &map));
        CHECK(map[kIndexSmooth] == 7);
        CHECK(map[kIndexCameraModel] == 8);
        CHECK(map[kIndexZoom] == 9);
        CHECK(map[kIndexDjiFov] == 10);
        CHECK(map[kIndexCorrection] == 11);
        CHECK(map[kIndexDragSensitivity] == 12);
        CHECK(map[kIndexLens] == 13);
        CHECK(map[kIndexKeyframeEasing] == 14);  // [WP-EASING]
        CHECK(map[kIndexSourcePan] == -1);
    }
    SECTION("a list of nothing but appended controls is not our list") {
        const std::vector<HostParamKind> tail(kValueParamKind + kOriginalValueParamCount,
                                              kValueParamKind + kValueParamCount);
        CHECK_FALSE(matchHostParams(tail.data(), static_cast<int>(tail.size()), &map));
    }
    SECTION("a list longer than every value control is not our list") {
        std::vector<HostParamKind> tooLong(kValueParamKind, kValueParamKind + kValueParamCount);
        tooLong.push_back(HostParamKind::Float64);
        CHECK_FALSE(matchHostParams(tooLong.data(), static_cast<int>(tooLong.size()), &map));
    }
}

// ===========================================================================
//  7. The GPU filter on DJI's lens
// ===========================================================================
namespace {

constexpr csSDK_int32 kNode = 7171;
constexpr PrTimelineID kTimeline = 0x7172;
constexpr PrTime kFrame30 = osv::premiere::mock::kTicksPerSecond / 30;

/// The panorama the GPU renders sample (smaller than the CPU one: uploads).
[[nodiscard]] const Panorama& gpuPanorama() {
    static const Panorama p = makePanorama(1024, 512);
    return p;
}

/// A CPU render of `s` from the GPU panorama, as RGBA floats.
[[nodiscard]] std::vector<float> cpuReference(const Settings& s, int w, int h) {
    const std::vector<std::uint8_t> bytes = renderSettings(s, w, h, gpuPanorama());
    std::vector<float> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            readPixelBgra32f(bytes.data(), w * 16, x, y,
                             rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x) * 4u);
        }
    }
    return rgba;
}

/// Store a value at a control's verbatim host index (AE index - 1).
void putParam(MockHost& host, int aeIndex, PrParamType type, double value) {
    PrParam p{};
    p.mType = type;
    switch (type) {
        case kPrParamType_Int32: p.mInt32 = static_cast<int>(value); break;
        case kPrParamType_Float64: p.mFloat64 = value; break;
        case kPrParamType_Float32: p.mFloat32 = static_cast<float>(value); break;
        default: p.mBool = (value != 0.0) ? 1 : 0; break;
    }
    host.setParam(kNode, gpuParamIndex(aeIndex), 0, p);
}

/// One GPU render through the filter of whatever the host holds on kNode.
[[nodiscard]] std::vector<float> gpuRender(MockHost& host, int w, int h) {
    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame in(host, gpuPanorama().width, gpuPanorama().height, false);
    GpuFrame out(host, w, h, false);
    REQUIRE(in.valid());
    REQUIRE(out.valid());
    REQUIRE(in.upload(packBgra32f(gpuPanorama(), in.rowBytes())));
    FilterInstance instance(scope, host, kNode, kTimeline);
    REQUIRE(instance.created() == suiteError_NoError);
    REQUIRE(instance.render(in, out, 10 * kFrame30, kFrame30) == suiteError_NoError);
    return out.downloadRgba();
}

#define DJI_REQUIRE_GPU(host)                                                                                  \
    do {                                                                                                       \
        if (!(host).gpuAvailable()) {                                                                          \
            SKIP("no CUDA device: " << (host).gpuFailureReason());                                             \
        }                                                                                                      \
    } while (0)

}  // namespace

TEST_CASE("the GPU filter renders DJI's lens exactly as the CPU path", "[reframe][gpu][cuda][dji]") {
    struct Lens {
        const char* name;
        double fov;
        double correction;
    };
    const Lens lenses[] = {{"103.3 / 0.67", 103.3, 0.67}, {"150 / 0.34", 150.0, 0.34}, {"crystal ball", 75.0, 1.8}};
    for (const Lens& l : lenses) {
        INFO(l.name);
        MockHost host;
        DJI_REQUIRE_GPU(host);
        // Premiere's verbatim layout, plus the DJI block on DJI's lens.
        Controls c;
        c.pan = 144.8;
        c.tilt = -5.9;
        writeVerbatimControls(host, kNode, c);
        // [WP-LENSUI] DJI selected by the Lens popup (1-based, like the rest
        // of this list) - and the hidden checkbox left unticked, so a filter
        // that still read the checkbox would render Classic and fail below.
        putParam(host, kIndexLens, kPrParamType_Int32, static_cast<double>(LensPopup::Dji));
        putParam(host, kIndexCameraModel, kPrParamType_Bool, 0.0);
        putParam(host, kIndexZoom, kPrParamType_Float64, 200.0);
        putParam(host, kIndexDjiFov, kPrParamType_Float64, l.fov);
        putParam(host, kIndexCorrection, kPrParamType_Float64, l.correction);
        putParam(host, kIndexDragSensitivity, kPrParamType_Float64, 2.0);

        const std::vector<float> gpu = gpuRender(host, 640, 360);
        REQUIRE(!gpu.empty());
        Settings s = settingsOf(c);
        s.cameraModel = CameraModel::Dji;
        s.djiFovDeg = l.fov;
        s.correction = l.correction;
        const double db = psnr(gpu, cpuReference(s, 640, 360));
        INFO("GPU vs CPU PSNR " << db << " dB");
        CHECK(db >= 60.0);
        // Rendering the Classic lens instead would be a different picture:
        // the Lens popup really was read.
        Settings classic = settingsOf(c);
        CHECK(psnr(gpu, cpuReference(classic, 640, 360)) < 30.0);
    }
}

TEST_CASE("the GPU filter reads popups from a 0-based host and a 1-based one alike", "[reframe][gpu][cuda][dji]") {
    // A square frame, so "Match Sequence" (the frame's own shape here) and
    // "3840 x 2160" (a 16:9 cover-fit) are different framings: decoding the
    // Output Resolution popup one entry off would show.
    constexpr int kW = 360;
    constexpr int kH = 360;
    Settings uhd = classicSettings(100.0, 20.0, 30.0, 5.0, 0.0, Resolution::Uhd3840x2160);
    Settings match = classicSettings(100.0, 20.0, 30.0, 5.0, 0.0, Resolution::MatchSequence);
    REQUIRE(psnr(cpuReference(uhd, kW, kH), cpuReference(match, kW, kH)) < 40.0);

    struct HostCase {
        const char* name;
        int resolution;  // raw popup values as the host serves them
        int preset;
        int lens;        // [WP-LENSUI] "Classic" in the same numbering
        const Settings* expected;
    };
    const HostCase cases[] = {
        // Premiere 26.2.2 counts from 0: 1 is "3840 x 2160", 0 is "Custom"
        // and 1 is "Classic".
        {"0-based, settled by Preset 0", 1, 0, 1, &uhd},
        // ...and its Match Sequence default reads 0.
        {"0-based, settled by Output Resolution 0", 0, 3, 1, &match},
        // After Effects' numbering: 2 is "3840 x 2160", 6 (the entry count)
        // is "Dewarping" and settles it; 2 is "Classic".
        {"1-based, settled by Preset 6", 2, 6, 2, &uhd},
    };
    for (const HostCase& hc : cases) {
        INFO(hc.name);
        MockHost host;
        DJI_REQUIRE_GPU(host);
        Controls c;
        c.resolution = hc.resolution;
        c.preset = hc.preset;
        c.lens = hc.lens;
        c.pan = 30.0;
        c.tilt = 5.0;
        c.fov = 100.0;
        c.distortion = 20.0;
        writeVerbatimControls(host, kNode, c);
        const std::vector<float> gpu = gpuRender(host, kW, kH);
        REQUIRE(!gpu.empty());
        const double db = psnr(gpu, cpuReference(*hc.expected, kW, kH));
        INFO("PSNR " << db << " dB");
        CHECK(db >= 60.0);
    }
}

// ===========================================================================
//  8. [WP-LENSUI] The Lens popup, end to end
// ===========================================================================
namespace {

/// The Settings the renderer builds from a USER_CHANGED_PARAM array - the
/// lens the popup selects and every control that lens reads - so a test can
/// build the camera the host would render after the effect edited the array.
[[nodiscard]] Settings settingsFromParams(Params& p) {
    Settings s;
    s.resolution = sanitiseResolution(p.at(kIndexOutputResolution).u.pd.value);
    s.preset = sanitisePreset(p.at(kIndexPreset).u.pd.value);
    s.cameraModel = cameraModelFromLensPopup(p.at(kIndexLens).u.pd.value);
    s.fovDeg = p.slider(kIndexFov);
    s.distortion = p.slider(kIndexDistortion);
    s.djiFovDeg = p.slider(kIndexDjiFov);
    s.correction = p.slider(kIndexCorrection);
    s.panDeg = static_cast<double>(p.at(kIndexPan).u.ad.value) / 65536.0;
    s.tiltDeg = static_cast<double>(p.at(kIndexTilt).u.ad.value) / 65536.0;
    s.rollDeg = static_cast<double>(p.at(kIndexRoll).u.ad.value) / 65536.0;
    return s;
}

/// Set an angle control of a USER_CHANGED_PARAM array, in degrees.
void setAngleDeg(Params& p, int aeIndex, double degrees) {
    p.at(aeIndex).u.ad.value = static_cast<PF_Fixed>(std::lround(degrees * 65536.0));
}

/// Store one registered control's value in the mock host, the way a saved
/// project hands it back: the registered def with `write` applied.
template <typename Write>
void storeValue(ModuleFixture& f, int aeIndex, Write&& write) {
    PF_ParamDef def = f.registered[static_cast<std::size_t>(aeIndex) - 1u];
    write(def);
    f.host.setParamValue(f.ref, aeIndex, def);
}

}  // namespace

TEST_CASE("switching the Lens popup keeps the framing on screen", "[reframe][supervise][dji][lens]") {
    ModuleFixture f;

    SECTION("Classic -> DJI frames exactly what Classic framed") {
        // The defaults, two preset looks, the automatic ramp and a wide
        // stereographic look, each aimed away from the centre.
        const ClassicLens looks[] = {{120.0, 15.0}, {150.0, 40.0}, {95.0, 0.0}, {210.0, 0.0}, {300.0, 100.0}};
        for (const ClassicLens& look : looks) {
            INFO("classic " << look.fovDeg << " / " << look.distortion);
            Params p(f.registered);
            p.setLens(CameraModel::Classic);
            p.at(kIndexFov).u.fs_d.value = static_cast<PF_FpShort>(look.fovDeg);
            p.at(kIndexDistortion).u.fs_d.value = static_cast<PF_FpShort>(look.distortion);
            setAngleDeg(p, kIndexPan, 25.0);
            setAngleDeg(p, kIndexTilt, -10.0);
            setAngleDeg(p, kIndexRoll, 3.0);
            const ViewSetup before = buildView(settingsFromParams(p), 1920, 1080, SizePx{});
            REQUIRE(before.valid);
            REQUIRE(before.params.projection == OSV_PROJ_EYE_OFFSET);

            // The user picks DJI.
            p.at(kIndexLens).u.pd.value = static_cast<A_long>(LensPopup::Dji);
            userChanged(f, p, kIndexLens);
            const ViewSetup after = buildView(settingsFromParams(p), 1920, 1080, SizePx{});
            REQUIRE(after.valid);
            REQUIRE(after.params.projection == OSV_PROJ_DJI_SPHERE);

            // The controls hold single-precision floats, so the carried lens
            // is the exact conversion rounded to a float - far below a pixel.
            const double worst = worstRayDifference(before.params, after.params);
            INFO("worst ray difference " << worst << " rad");
            CHECK(worst >= 0.0);
            CHECK(worst < 2e-4);
        }
    }
    SECTION("DJI -> Classic frames what DJI framed whenever Classic can express it") {
        // DJI's Wide and Dewarping: correction at most 1 and no Classic ramp
        // above it, so the Classic look is exact.
        const DjiLens lenses[] = {{60.0, 0.6}, {80.0, 0.2}};
        for (const DjiLens& lens : lenses) {
            INFO("dji " << lens.fovDeg << " / " << lens.correction);
            Params p(f.registered);
            p.setLens(CameraModel::Dji);
            p.at(kIndexDjiFov).u.fs_d.value = static_cast<PF_FpShort>(lens.fovDeg);
            p.at(kIndexCorrection).u.fs_d.value = static_cast<PF_FpShort>(lens.correction);
            setAngleDeg(p, kIndexPan, -40.0);
            setAngleDeg(p, kIndexTilt, 12.0);
            const ViewSetup before = buildView(settingsFromParams(p), 1920, 1080, SizePx{});
            REQUIRE(before.valid);

            p.at(kIndexLens).u.pd.value = static_cast<A_long>(LensPopup::Classic);
            userChanged(f, p, kIndexLens);
            const ViewSetup after = buildView(settingsFromParams(p), 1920, 1080, SizePx{});
            REQUIRE(after.valid);
            REQUIRE(after.params.projection == OSV_PROJ_EYE_OFFSET);
            const double worst = worstRayDifference(before.params, after.params);
            INFO("worst ray difference " << worst << " rad");
            CHECK(worst >= 0.0);
            CHECK(worst < 2e-4);
        }
    }
    SECTION("a switch never touches the framing angles") {
        Params p(f.registered);
        p.setLens(CameraModel::Classic);
        setAngleDeg(p, kIndexPan, 144.8);
        setAngleDeg(p, kIndexTilt, -5.9);
        p.at(kIndexLens).u.pd.value = static_cast<A_long>(LensPopup::Dji);
        userChanged(f, p, kIndexLens);
        CHECK_FALSE(p.marked(kIndexPan));
        CHECK_FALSE(p.marked(kIndexTilt));
        CHECK_FALSE(p.marked(kIndexRoll));
        CHECK(static_cast<double>(p.at(kIndexPan).u.ad.value) / 65536.0 == Approx(144.8).margin(1e-4));
    }
}

TEST_CASE("an old project opens on the DJI lens, whatever its checkbox held", "[reframe][cpu][dji][lens][legacy]") {
    // A project saved before the Lens popup existed has no value for it, so
    // the host restores the popup's default - DJI.  The WP-CAMERA checkbox it
    // DID save is the popup's hidden mirror now and is never rendered from:
    // ticked (DJI) or unticked (Classic), the picture is DJI's.  The user
    // accepted that a Classic project of that era opens on DJI.
    constexpr int kW = 320;
    constexpr int kH = 180;
    const Settings dji = djiSettings(103.3, 0.67, 144.8, -5.9, 0.0, Resolution::Fhd1920x1080);
    const Settings classic = classicSettings(120.0, 15.0, 144.8, -5.9, 0.0, Resolution::Fhd1920x1080);
    const std::vector<float> djiReference = renderSettingsRgba(dji, kW, kH);
    REQUIRE(psnr(djiReference, renderSettingsRgba(classic, kW, kH)) < 30.0);

    for (const bool ticked : {true, false}) {
        INFO("saved checkbox " << (ticked ? "ticked" : "unticked"));
        ModuleFixture f;
        // The saved controls.  The Lens popup is left exactly as the host
        // restores a parameter the project does not have: at its default.
        storeValue(f, kIndexCameraModel, [&](PF_ParamDef& d) { d.u.bd.value = ticked ? 1 : 0; });
        storeValue(f, kIndexOutputResolution,
                   [](PF_ParamDef& d) { d.u.pd.value = static_cast<A_long>(Resolution::Fhd1920x1080); });
        storeValue(f, kIndexPreset, [](PF_ParamDef& d) { d.u.pd.value = static_cast<A_long>(Preset::Custom); });
        storeValue(f, kIndexFov, [](PF_ParamDef& d) { d.u.fs_d.value = 120.0f; });
        storeValue(f, kIndexDistortion, [](PF_ParamDef& d) { d.u.fs_d.value = 15.0f; });
        storeValue(f, kIndexDjiFov, [](PF_ParamDef& d) { d.u.fs_d.value = 103.3f; });
        storeValue(f, kIndexCorrection, [](PF_ParamDef& d) { d.u.fs_d.value = 0.67f; });
        storeValue(f, kIndexPan, [](PF_ParamDef& d) { d.u.ad.value = static_cast<PF_Fixed>(std::lround(144.8 * 65536.0)); });
        storeValue(f, kIndexTilt, [](PF_ParamDef& d) { d.u.ad.value = static_cast<PF_Fixed>(std::lround(-5.9 * 65536.0)); });
        REQUIRE(f.host.addedParams(f.ref)[kIndexLens - 1].u.pd.value == OSV_REFRAME_LENS_DEFAULT);

        // Render through the module's CPU path.
        const Panorama& pano = panorama();
        std::unique_ptr<osv::premiere::mock::EffectWorld> input =
            f.host.createWorld(static_cast<std::uint32_t>(pano.width), static_cast<std::uint32_t>(pano.height),
                               PrPixelFormat_BGRA_4444_32f);
        REQUIRE(input != nullptr);
        const std::vector<std::uint8_t> packed = packBgra32f(pano, input->rowBytes());
        REQUIRE(packed.size() <= static_cast<std::size_t>(input->rowBytes()) * static_cast<std::size_t>(pano.height));
        std::memcpy(input->pixels(), packed.data(), packed.size());
        f.host.setInputWorld(f.ref, input.get());
        std::unique_ptr<osv::premiere::mock::EffectWorld> output =
            f.host.createWorld(kW, kH, PrPixelFormat_BGRA_4444_32f);
        REQUIRE(output != nullptr);
        std::vector<PF_ParamDef*> params = f.host.renderParams(f.ref);
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_RENDER, &f.in, &f.out, params.data(), &output->world(),
                                                      nullptr) == PF_Err_NONE);

        std::vector<float> rendered(static_cast<std::size_t>(kW) * kH * 4u, 0.0f);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                readPixelBgra32f(reinterpret_cast<const std::uint8_t*>(output->pixels()), output->rowBytes(), x, y,
                                 rendered.data() + (static_cast<std::size_t>(y) * kW + x) * 4u);
            }
        }
        const double db = psnr(rendered, djiReference);
        INFO("module render vs the DJI reference: " << db << " dB");
        CHECK(db >= 60.0);
    }
}

TEST_CASE("the GPU filter reads the Lens popup, not the old checkbox, in either numbering",
          "[reframe][gpu][cuda][dji][lens]") {
    // A square frame, so a one-entry misread of Output Resolution would show
    // as well as a misread of the lens.
    constexpr int kW = 360;
    constexpr int kH = 360;
    const Settings djiUhd = djiSettings(90.0, 0.5, 30.0, 5.0, 0.0, Resolution::Uhd3840x2160);
    const Settings classicUhd = classicSettings(100.0, 20.0, 30.0, 5.0, 0.0, Resolution::Uhd3840x2160);
    REQUIRE(psnr(cpuReference(djiUhd, kW, kH), cpuReference(classicUhd, kW, kH)) < 40.0);

    struct HostCase {
        const char* name;
        int resolution;  // raw values, as the host serves them
        int preset;
        int lens;
        bool checkbox;   // the hidden mirror
        const Settings* expected;
    };
    const HostCase cases[] = {
        // Premiere 26.2.2 numbers from 0: 1 is "3840 x 2160", 3 is "Wide"
        // (ambiguous on its own) - and the Lens popup's DJI reads 0, which
        // settles the numbering by itself.  An old project on Premiere looks
        // exactly like this: the restored default reads 0, and the checkbox
        // it saved says whatever it said.
        {"0-based, DJI (0) settles the base by itself", 1, 3, 0, false, &djiUhd},
        {"0-based, an old project whose checkbox was ticked", 1, 3, 0, true, &djiUhd},
        // Classic reads 1 on Premiere, which alone would be ambiguous - but
        // the effect only ever leaves Classic beside Preset "Custom" (0).
        // The mirror is set to DISAGREE, so reading it would fail.
        {"0-based, Classic (1) beside Custom (0)", 1, 0, 1, true, &classicUhd},
        // After Effects' numbering (the mock's): DJI reads 1 and nothing
        // settles the base, which then reads 1-based...
        {"1-based, DJI (1) with nothing else settling", 2, 4, 1, false, &djiUhd},
        // ...and Classic reads 2, the popup's entry count, which settles it.
        {"1-based, Classic (2) settles the base by itself", 2, 4, 2, true, &classicUhd},
    };
    for (const HostCase& hc : cases) {
        INFO(hc.name);
        MockHost host;
        DJI_REQUIRE_GPU(host);
        Controls c;
        c.resolution = hc.resolution;
        c.preset = hc.preset;
        c.lens = hc.lens;
        c.pan = 30.0;
        c.tilt = 5.0;
        c.fov = 100.0;
        c.distortion = 20.0;
        writeVerbatimControls(host, kNode, c);
        putParam(host, kIndexCameraModel, kPrParamType_Bool, hc.checkbox ? 1.0 : 0.0);
        putParam(host, kIndexDjiFov, kPrParamType_Float64, 90.0);
        putParam(host, kIndexCorrection, kPrParamType_Float64, 0.5);
        const std::vector<float> gpu = gpuRender(host, kW, kH);
        REQUIRE(!gpu.empty());
        const double db = psnr(gpu, cpuReference(*hc.expected, kW, kH));
        INFO("PSNR against the expected lens " << db << " dB");
        CHECK(db >= 60.0);
        // And the other lens is a different picture: the popup decided it.
        const Settings& other = (hc.expected == &djiUhd) ? classicUhd : djiUhd;
        CHECK(psnr(gpu, cpuReference(other, kW, kH)) < 40.0);
    }
}
