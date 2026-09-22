// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Parallax warp tests: the 2-D flow-based seam correction (ParallaxWarp.h)
// and the kernel path that applies it (osvShadePixelW / osvWarpSample).
//
// What is pinned here, and why each one matters:
//
//   * DIRECTION.  The first version of the kernel moved both lenses the SAME
//     way - a sign convention that looked symmetric and was not - so the
//     "correction" slid the picture without closing any disparity.  A test
//     that renders each lens with a constant warp and checks which way its
//     band moved would have caught that on the first run; it is here now.
//
//   * A KNOWN 2-D PARALLAX IS RECOVERED AND REMOVED.  Synthetic fisheye
//     frames of one procedural scene, with the master lens seeing it shifted
//     in both latitude AND longitude, through the real rig geometry.  The
//     longitude part is exactly what a 1-D seam table cannot express.
//
//   * NOTHING CHANGES WHEN THERE IS NOTHING TO FIX.  A zero grid renders
//     bit-identically to no grid at all.
//
//   * BOUNDARY DECAY.  The correction reaches exactly zero at the grid's
//     latitude edges; anything else is a tear at the band edge.
//
//   * THE BENEFIT GATE.  A correction that does not make the lenses agree
//     is not applied - the sample clip's wingtip, where the two lenses see
//     different objects, is the real-world case and has its own test.

#include <catch2/catch_test_macros.hpp>

#include "TestSample.h"

#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/core/Math.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/DualStreamReader.h"
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaRenderer.h"
#endif
#if defined(OSV_HAVE_OPENCL)
#include "osv/render/OpenClRenderer.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace osv;

namespace {

// ---------------------------------------------------------------------------
//  Synthetic rig and scene
// ---------------------------------------------------------------------------

/// The sample clip's verified calibration as a stream-space rig of the given
/// width.  Same constants as test_render.cpp, so these tests exercise the
/// real lens geometry - including the small misalignment of the two optical
/// axes - without needing the clip.
Result<geom::LensRig> makeSampleRig(int streamW) {
    meta::CalibrationSet cal;
    auto fill = [](meta::DewarpParams& d, float fx, float fy, float cx, float cy, const float k[5], float qw, float qx,
                   float qy, float qz) {
        d.fx = fx;
        d.fy = fy;
        d.cx = cx;
        d.cy = cy;
        for (int i = 0; i < 5; ++i) {
            d.k[static_cast<std::size_t>(i)] = k[i];
        }
        d.width = 3840;
        d.height = 3840;
        d.camExtriQ.w = qw;
        d.camExtriQ.x = qx;
        d.camExtriQ.y = qy;
        d.camExtriQ.z = qz;
        d.camExtriQ.present = true;
        for (int b = 1; b <= 11; ++b) {
            d.present.set(static_cast<std::size_t>(b));
        }
        d.present.set(28);
    };
    const float ks[5] = {0.0667397f, -0.0128859f, 0.0103815f, -0.00677581f, 0.00098791f};
    const float km[5] = {0.0613421f, -0.00480161f, 0.00444291f, -0.00452633f, 0.00066212f};
    fill(cal.slave, 1043.8802f, 1043.6731f, 1917.0421f, 1919.1294f, ks, 0.0026615f, 0.0019091f, -0.7056412f, 0.7085618f);
    fill(cal.master, 1043.0103f, 1042.9268f, 1908.8036f, 1918.7257f, km, 0.7036960f, 0.7103991f, -0.0046943f,
         -0.0110939f);
    cal.sourceSlave = "test";
    cal.sourceMaster = "test";
    const double dfl = 829.3612 * (streamW / 3000.0);
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(streamW, streamW, 3840, 3840, dfl, 1043.445, streamW / 3776.0, nullptr));
    return geom::LensRig::build(cal, scaling, geom::FocalSource::DigitalFocalLength, dfl, geom::ExtrinsicConvention{});
}

/// Procedural scene texture in [0.14, 0.86] as a function of the band's own
/// polar-axis coordinates (lon = atan2(x, z), lat = asin(y), radians).
///
/// A sum of sinusoids with incommensurate wave vectors rather than a single
/// grating: a periodic pattern matches itself one period away, and a flow
/// solver that locks onto the wrong period would make this test measure the
/// texture instead of the warp.  Feature scale is 4-7 degrees, several band
/// pixels at the resolutions used here.
double sceneTexture(double lon, double lat) noexcept {
    return 0.5 + 0.12 * std::sin(61.0 * lon + 47.0 * lat + 0.3) + 0.10 * std::sin(37.0 * lon - 83.0 * lat + 1.1) +
           0.08 * std::sin(71.0 * lat + 2.3 * std::sin(9.0 * lon)) + 0.06 * std::sin(97.0 * lon + 2.0);
}

/// Owns planar 10-bit YCbCr storage for one synthetic lens frame.
struct SyntheticFrame {
    std::shared_ptr<std::vector<std::uint16_t>> storage;
    video::PlanarFrame16 frame;
};

/// Render the procedural scene into lens `lens` of `rig`, as that lens would
/// see it if the scene were displaced by (+dLon, +dLat) radians - i.e. the
/// content at (L, P) appears in this lens at (L + dLon, P + dLat).  Giving
/// only the master a displacement is a known, uniform 2-D parallax.
SyntheticFrame makeSceneFrame(const geom::LensRig& rig, int lens, double dLon, double dLat) {
    SyntheticFrame s;
    const std::uint32_t w = static_cast<std::uint32_t>(rig.streamW);
    const std::uint32_t h = static_cast<std::uint32_t>(rig.streamH);
    const std::uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    const std::size_t lumaN = static_cast<std::size_t>(w) * h;
    const std::size_t chromaN = static_cast<std::size_t>(cw) * ch;
    s.storage = std::make_shared<std::vector<std::uint16_t>>(lumaN + 2 * chromaN, static_cast<std::uint16_t>(512));
    std::uint16_t* base = s.storage->data();
    const Mat3d lensToBody = rig.bodyToLens[static_cast<std::size_t>(lens)].transposed();
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            double v = 0.5;
            // Pixel centre, in the same continuous coordinates project() uses.
            auto dirLens = rig.lens[static_cast<std::size_t>(lens)].unproject(Vec2d{x + 0.5, y + 0.5});
            if (dirLens.ok()) {
                const Vec3d d = lensToBody * dirLens.value();
                const double lon = std::atan2(d.x, d.z);
                const double lat = std::asin(std::clamp(d.y, -1.0, 1.0));
                v = sceneTexture(lon - dLon, lat - dLat);
            }
            // Narrow-range 10-bit luma; chroma stays neutral (512).
            base[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint16_t>(std::lround(64.0 + v * 876.0));
        }
    }
    s.frame.width = w;
    s.frame.height = h;
    s.frame.chromaW = cw;
    s.frame.chromaH = ch;
    s.frame.plane = {base, base + lumaN, base + lumaN + chromaN};
    s.frame.strideElems = {w, cw, cw};
    s.frame.bitDepth = 10;
    s.frame.bitShift = 0;
    s.frame.chromaInterleaved = false;
    s.frame.narrowRange = true;
    s.frame.owner = s.storage;
    return s;
}

/// A frame pair of the procedural scene with the master displaced by
/// (dLon, dLat) radians.  The SyntheticFrame owners are kept inside the
/// PlanarFrame16 `owner` pointers, so the pair is self-contained.
video::FramePair makeScenePair(const geom::LensRig& rig, double dLon, double dLat) {
    video::FramePair pair;
    pair.lens[0] = makeSceneFrame(rig, 0, 0.0, 0.0).frame;
    pair.lens[1] = makeSceneFrame(rig, 1, dLon, dLat).frame;
    return pair;
}

/// Build the synthetic rig and scene once: unprojecting two frames costs
/// far more than any single assertion, and several tests share them.
struct SceneFixture {
    geom::LensRig rig;
    video::FramePair flat;      ///< No parallax.
    video::FramePair parallax;  ///< Master displaced by kParallaxLon / kParallaxLat.
    bool ok = false;
};

constexpr double kParallaxLatDeg = 1.0;  ///< Along-meridian disparity (what a seam table can fix).
constexpr double kParallaxLonDeg = 0.6;  ///< Cross-meridian disparity (what it cannot).
constexpr int kStreamW = 1024;

const SceneFixture& scene() {
    static const SceneFixture fixture = [] {
        SceneFixture f;
        auto rig = makeSampleRig(kStreamW);
        if (!rig.ok()) {
            return f;
        }
        f.rig = std::move(rig).value();
        f.flat = makeScenePair(f.rig, 0.0, 0.0);
        f.parallax = makeScenePair(f.rig, deg2rad(kParallaxLonDeg), deg2rad(kParallaxLatDeg));
        f.ok = f.flat.valid() && f.parallax.valid();
        return f;
    }();
    return fixture;
}

/// Band geometry matched to the synthetic frames' resolution (~0.35 deg per
/// band pixel against ~0.2 deg per fisheye pixel at the rim).
render::BandParams syntheticBand(double halfDeg) { return render::BandParams{1024, halfDeg}; }

/// Parallax parameters for the synthetic tests: classical flow explicitly,
/// so the result does not depend on whether a neural model is installed.
render::ParallaxWarpParams syntheticParams() {
    render::ParallaxWarpParams p;
    p.backend = render::FlowBackendKind::Classical;
    p.band = syntheticBand(6.0);
    return p;
}

/// Median of a copy of `v` (empty -> NaN).
double median(std::vector<double> v) {
    if (v.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

/// Synthetic LensBands with full coverage and a given luma function of the
/// band pixel (x, y).  Geometry mimics renderLensBands: a band centred on the
/// equator row of a `mapH`-row polar-axis map.
template <class LumaA, class LumaB>
render::LensBands makeBands(std::uint32_t w, std::uint32_t h, std::uint32_t mapH, LumaA lumaA, LumaB lumaB) {
    render::LensBands b;
    b.w = w;
    b.h = h;
    b.mapH = mapH;
    b.rowOffset = mapH / 2 - h / 2;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    for (int lens = 0; lens < 2; ++lens) {
        b.luma[lens].resize(n);
        b.alpha[lens].assign(n, 1.0f);
    }
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            b.luma[0][static_cast<std::size_t>(y) * w + x] = static_cast<float>(lumaA(x, y));
            b.luma[1][static_cast<std::size_t>(y) * w + x] = static_cast<float>(lumaB(x, y));
        }
    }
    return b;
}

/// A uniform bidirectional flow: forward (u, v), backward (-u, -v), all ok.
render::BidirFlow uniformFlow(std::uint32_t w, std::uint32_t h, float u, float v) {
    render::BidirFlow f;
    f.forward.resize(w, h);
    f.backward.resize(w, h);
    std::fill(f.forward.u.begin(), f.forward.u.end(), u);
    std::fill(f.forward.v.begin(), f.forward.v.end(), v);
    std::fill(f.backward.u.begin(), f.backward.u.end(), -u);
    std::fill(f.backward.v.begin(), f.backward.v.end(), -v);
    f.ok.assign(static_cast<std::size_t>(w) * h, 1u);
    f.consistent = f.ok.size();
    return f;
}

/// Band-pixel texture for the host-side tests (wraps in x like the band).
double bandTexture(double x, double y, std::uint32_t w) noexcept {
    const double t = osv::kTwoPi * x / static_cast<double>(w);
    return 0.5 + 0.2 * std::sin(23.0 * t + 0.37 * y) + 0.15 * std::sin(0.61 * y - 11.0 * t + 0.4) +
           0.1 * std::sin(41.0 * t + 0.23 * y + 1.7);
}

/// Value of component `comp` of grid cell (x, y).
float cell(const render::ParallaxWarpGrid& g, std::uint32_t x, std::uint32_t y, int comp) {
    return g.uv[(static_cast<std::size_t>(y) * g.w + x) * 2u + static_cast<std::size_t>(comp)];
}

/// Largest |value| of either component on grid row y.
double rowMaxAbs(const render::ParallaxWarpGrid& g, std::uint32_t y) {
    double m = 0.0;
    for (std::uint32_t x = 0; x < g.w; ++x) {
        m = std::max({m, std::fabs(static_cast<double>(cell(g, x, y, 0))),
                      std::fabs(static_cast<double>(cell(g, x, y, 1)))});
    }
    return m;
}

/// Mean |a(r + dr, c + dc) - b(r, c)| over the band pixels where both bands
/// are fully covered and the shifted row stays inside.  Columns wrap.
double shiftedMeanAbsDiff(const render::LensBands& a, int lensA, const render::LensBands& b, int lensB, int dr,
                          int dc) {
    double sum = 0.0;
    std::size_t n = 0;
    const int W = static_cast<int>(a.w);
    for (int r = 0; r < static_cast<int>(a.h); ++r) {
        const int rs = r + dr;
        if (rs < 0 || rs >= static_cast<int>(a.h)) {
            continue;
        }
        for (int c = 0; c < W; ++c) {
            const int cs = ((c + dc) % W + W) % W;
            const std::size_t ia = static_cast<std::size_t>(rs) * a.w + static_cast<std::size_t>(cs);
            const std::size_t ib = static_cast<std::size_t>(r) * b.w + static_cast<std::size_t>(c);
            if (a.alpha[lensA][ia] > 0.99f && b.alpha[lensB][ib] > 0.99f) {
                sum += std::fabs(static_cast<double>(a.luma[lensA][ia]) - b.luma[lensB][ib]);
                ++n;
            }
        }
    }
    return n ? sum / static_cast<double>(n) : std::numeric_limits<double>::infinity();
}

/// NCC over band columns [c0, c1] (wrapping), co-visible pixels only - the
/// region version of render::overlapNcc.
double regionNcc(const render::LensBands& b, int c0, int c1) {
    std::vector<double> xa, xb;
    const int W = static_cast<int>(b.w);
    const int span = c1 >= c0 ? c1 - c0 + 1 : (W - c0) + c1 + 1;
    for (std::uint32_t r = 0; r < b.h; ++r) {
        for (int k = 0; k < span; ++k) {
            const int c = ((c0 + k) % W + W) % W;
            const std::size_t i = static_cast<std::size_t>(r) * b.w + static_cast<std::size_t>(c);
            if (b.alpha[0][i] > 0.5f && b.alpha[1][i] > 0.5f) {
                xa.push_back(b.luma[0][i]);
                xb.push_back(b.luma[1][i]);
            }
        }
    }
    if (xa.size() < 16) {
        return 0.0;
    }
    double ma = 0, mb = 0;
    for (std::size_t i = 0; i < xa.size(); ++i) {
        ma += xa[i];
        mb += xb[i];
    }
    ma /= static_cast<double>(xa.size());
    mb /= static_cast<double>(xa.size());
    double num = 0, da = 0, db = 0;
    for (std::size_t i = 0; i < xa.size(); ++i) {
        num += (xa[i] - ma) * (xb[i] - mb);
        da += (xa[i] - ma) * (xa[i] - ma);
        db += (xb[i] - mb) * (xb[i] - mb);
    }
    return (da > 0 && db > 0) ? num / std::sqrt(da * db) : 0.0;
}

/// A WarpGridView over a ParallaxWarpGrid (no copy).
render::WarpGridView viewOf(const render::ParallaxWarpGrid& g) {
    render::WarpGridView v;
    v.uv = g.uv.data();
    v.w = g.w;
    v.h = g.h;
    v.latMinRad = g.latMinRad;
    v.latMaxRad = g.latMaxRad;
    return v;
}

}  // namespace

// ===========================================================================
//  Kernel: direction and identity
// ===========================================================================

TEST_CASE("the kernel moves the two lenses in OPPOSITE directions by the grid's amount", "[render][parallax]") {
    const SceneFixture& s = scene();
    REQUIRE(s.ok);
    ThreadPool pool;
    geom::BlendParams blend;
    const render::BandParams band = syntheticBand(6.0);

    auto plain = render::renderLensBands(s.rig, s.flat, blend, band, false, nullptr, pool);
    REQUIRE(plain.ok());
    const std::uint32_t mapH = plain.value().mapH;
    const double radPerRow = osv::kPi / static_cast<double>(mapH);
    const double radPerCol = osv::kTwoPi / static_cast<double>(plain.value().w);

    // A constant grid spanning well past the band, so every band ray gets
    // exactly the same displacement.
    const auto constantGrid = [](float dLon, float dLat) {
        std::vector<float> uv(16u * 4u * 2u);
        for (std::size_t i = 0; i < uv.size(); i += 2) {
            uv[i] = dLon;
            uv[i + 1] = dLat;
        }
        return uv;
    };

    SECTION("along the meridian: the master moves by +dLat, the slave by -dLat") {
        constexpr int k = 3;  // rows
        const std::vector<float> uv = constantGrid(0.0f, static_cast<float>(k * radPerRow));
        render::WarpGridView view{uv.data(), 16, 4, static_cast<float>(deg2rad(20.0)),
                                  static_cast<float>(deg2rad(-20.0))};
        auto warped = render::renderLensBands(s.rig, s.flat, blend, band, false, nullptr, pool, &view);
        REQUIRE(warped.ok());

        // The master samples at lat + dLat, i.e. k rows UP (row 0 is the
        // +90 pole): warped(r) = plain(r - k).  The slave samples at
        // lat - dLat: warped(r) = plain(r + k).
        const double masterRight = shiftedMeanAbsDiff(plain.value(), 1, warped.value(), 1, -k, 0);
        const double masterWrong = shiftedMeanAbsDiff(plain.value(), 1, warped.value(), 1, +k, 0);
        const double slaveRight = shiftedMeanAbsDiff(plain.value(), 0, warped.value(), 0, +k, 0);
        const double slaveWrong = shiftedMeanAbsDiff(plain.value(), 0, warped.value(), 0, -k, 0);
        INFO("master right " << masterRight << " wrong " << masterWrong << "; slave right " << slaveRight
                             << " wrong " << slaveWrong);
        REQUIRE(masterRight < 0.002);
        REQUIRE(slaveRight < 0.002);
        // The direction is what the first kernel got wrong: pin it by making
        // the opposite hypothesis clearly worse, not merely "not better".
        REQUIRE(masterWrong > 10.0 * masterRight);
        REQUIRE(slaveWrong > 10.0 * slaveRight);
    }

    SECTION("across the meridian: the master moves by +dLon, the slave by -dLon") {
        constexpr int m = 4;  // columns
        const std::vector<float> uv = constantGrid(static_cast<float>(m * radPerCol), 0.0f);
        render::WarpGridView view{uv.data(), 16, 4, static_cast<float>(deg2rad(20.0)),
                                  static_cast<float>(deg2rad(-20.0))};
        auto warped = render::renderLensBands(s.rig, s.flat, blend, band, false, nullptr, pool, &view);
        REQUIRE(warped.ok());
        // Master samples at lon + dLon: warped(c) = plain(c + m).
        const double masterRight = shiftedMeanAbsDiff(plain.value(), 1, warped.value(), 1, 0, +m);
        const double masterWrong = shiftedMeanAbsDiff(plain.value(), 1, warped.value(), 1, 0, -m);
        const double slaveRight = shiftedMeanAbsDiff(plain.value(), 0, warped.value(), 0, 0, -m);
        const double slaveWrong = shiftedMeanAbsDiff(plain.value(), 0, warped.value(), 0, 0, +m);
        INFO("master right " << masterRight << " wrong " << masterWrong << "; slave right " << slaveRight
                             << " wrong " << slaveWrong);
        REQUIRE(masterRight < 0.002);
        REQUIRE(slaveRight < 0.002);
        REQUIRE(masterWrong > 10.0 * masterRight);
        REQUIRE(slaveWrong > 10.0 * slaveRight);
    }
}

TEST_CASE("a zero warp grid renders bit-identically to no grid", "[render][parallax]") {
    const SceneFixture& s = scene();
    REQUIRE(s.ok);
    ThreadPool pool;
    render::CpuRenderer cpu(pool);
    geom::BlendParams blend;
    const OsvColorParams cp =
        color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Rec709, 0.0f);

    // A full Standard equirect: every direction the renderer can produce,
    // including the whole overlap band the grid covers.
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::Standard;
    map.w = 512;
    map.h = 256;

    render::RenderParamsBuilder plainBuilder;
    plainBuilder.rig(s.rig).equirect(map).blend(blend, true).color(cp);
    auto plainJob = plainBuilder.build(s.parallax);
    REQUIRE(plainJob.ok());
    REQUIRE(plainJob.value().params.warpEnabled == 0);

    const std::vector<float> zeros(32u * 8u * 2u, 0.0f);
    render::RenderParamsBuilder warpBuilder;
    warpBuilder.rig(s.rig).equirect(map).blend(blend, true).color(cp).warp(
        zeros, 32, 8, static_cast<float>(deg2rad(10.0)), static_cast<float>(deg2rad(-10.0)));
    auto warpJob = warpBuilder.build(s.parallax);
    REQUIRE(warpJob.ok());
    // The warp really is ON - otherwise this test would prove nothing.
    REQUIRE(warpJob.value().params.warpEnabled == 1);

    auto a = cpu.render(plainJob.value());
    auto b = cpu.render(warpJob.value());
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(a.value().data.size() == b.value().data.size());
    REQUIRE(std::memcmp(a.value().data.data(), b.value().data.data(), a.value().data.size() * sizeof(float)) == 0);
}

TEST_CASE("osvWarpSample wraps longitude, and is zero outside the grid's latitude span", "[render][parallax]") {
    // A 4 x 3 grid whose dLon value equals its column index: continuity
    // across the +/-180 meridian is then visible as the interpolation between
    // column 3 and column 0.
    std::vector<float> uv(4u * 3u * 2u, 0.0f);
    for (std::uint32_t y = 0; y < 3; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            uv[(y * 4u + x) * 2u] = static_cast<float>(x);
            uv[(y * 4u + x) * 2u + 1u] = 1.0f;
        }
    }
    OsvRenderParams p{};
    p.warpEnabled = 1;
    p.warpW = 4;
    p.warpH = 3;
    p.warpLatMinRad = 0.1f;   // row 0
    p.warpLatMaxRad = -0.1f;  // row 2

    // Column 0 sits at lon = -pi; halfway to column 1 is lon = -pi + pi/4.
    REQUIRE(std::fabs(osvWarpSample(&p, uv.data(), -OSV_KERNEL_PI, 0.0f, 0) - 0.0f) < 1e-5f);
    REQUIRE(std::fabs(osvWarpSample(&p, uv.data(), -OSV_KERNEL_PI + OSV_KERNEL_PI / 4.0f, 0.0f, 0) - 0.5f) < 1e-4f);
    // Just west of +pi interpolates column 3 toward column 0 (the wrap), not
    // toward a clamped column 3.
    const float nearWrap = osvWarpSample(&p, uv.data(), OSV_KERNEL_PI - OSV_KERNEL_PI / 4.0f, 0.0f, 0);
    REQUIRE(std::fabs(nearWrap - 1.5f) < 1e-3f);

    // Inside the latitude span the second component is 1; beyond it, 0.
    REQUIRE(std::fabs(osvWarpSample(&p, uv.data(), 0.0f, 0.05f, 1) - 1.0f) < 1e-6f);
    REQUIRE(osvWarpSample(&p, uv.data(), 0.0f, 0.11f, 1) == 0.0f);
    REQUIRE(osvWarpSample(&p, uv.data(), 0.0f, -0.11f, 1) == 0.0f);

    // Defensive: no grid, or no size, is no correction.
    REQUIRE(osvWarpSample(&p, nullptr, 0.0f, 0.0f, 1) == 0.0f);
    p.warpW = 0;
    REQUIRE(osvWarpSample(&p, uv.data(), 0.0f, 0.0f, 1) == 0.0f);
}

// ===========================================================================
//  Flow -> grid conversion
// ===========================================================================

TEST_CASE("gridFromFlow turns a uniform flow into the documented half disparity", "[render][parallax]") {
    constexpr std::uint32_t W = 512, H = 40, mapH = 256;
    const auto flat = [](std::uint32_t x, std::uint32_t y) { return bandTexture(x, y, W); };
    const render::LensBands bands = makeBands(W, H, mapH, flat, flat);
    // Forward flow (+2 columns, -3 rows): content moves east and UP (toward
    // the +90 pole) from the slave to the master.
    const render::BidirFlow flow = uniformFlow(W, H, 2.0f, -3.0f);

    render::ParallaxWarpParams p;
    p.gridW = 64;
    p.gridRows = 8;
    p.decayRows = 4;
    p.requiredImprovement = 0.0;  // judge the conversion alone, not the gate
    auto grid = render::gridFromFlow(bands, flow, p);
    REQUIRE(grid.ok());
    const render::ParallaxWarpGrid& g = grid.value();
    REQUIRE(g.valid());
    REQUIRE(g.w == 64);
    REQUIRE(g.h == 8 + 2 * 4);
    REQUIRE(g.consistentFraction() == 1.0);

    // Half of the disparity, in radians, as the master's displacement: east
    // is +longitude, and a row step UP is +latitude.
    const double radPerCol = osv::kTwoPi / W;
    const double radPerRow = osv::kPi / mapH;
    const double expectLon = 0.5 * 2.0 * radPerCol;
    const double expectLat = 0.5 * 3.0 * radPerRow;
    for (std::uint32_t y = p.decayRows; y < p.decayRows + p.gridRows; ++y) {
        for (std::uint32_t x = 0; x < g.w; ++x) {
            REQUIRE(std::fabs(cell(g, x, y, 0) - expectLon) < 1e-6 * std::fabs(expectLon) + 1e-9);
            REQUIRE(std::fabs(cell(g, x, y, 1) - expectLat) < 1e-6 * std::fabs(expectLat) + 1e-9);
        }
    }
    // Diagnostics report the FULL disparity.
    const double fullDeg = 2.0 * rad2deg(std::hypot(expectLon, expectLat));
    REQUIRE(std::fabs(g.meanAbsCorrectionDeg - fullDeg) < 1e-4);

    // Row mapping: the kernel must read the measured value at the latitude of
    // the band's first row, and nothing past the grid's own span.
    OsvRenderParams kp{};
    kp.warpEnabled = 1;
    kp.warpW = static_cast<int>(g.w);
    kp.warpH = static_cast<int>(g.h);
    kp.warpLatMinRad = g.latMinRad;
    kp.warpLatMaxRad = g.latMaxRad;
    const double latBandTop = osv::kHalfPi - (bands.rowOffset + 0.5) * radPerRow;
    const double latBandBottom = osv::kHalfPi - (bands.rowOffset + H - 0.5) * radPerRow;
    REQUIRE(g.latMinRad > g.latMaxRad);  // rows descend in latitude
    REQUIRE(std::fabs(osvWarpSample(&kp, g.uv.data(), 0.0f, static_cast<float>(latBandTop), 1) - expectLat) <
            1e-6);
    REQUIRE(std::fabs(osvWarpSample(&kp, g.uv.data(), 0.0f, static_cast<float>(latBandBottom), 1) - expectLat) <
            1e-6);
    REQUIRE(osvWarpSample(&kp, g.uv.data(), 0.0f, g.latMinRad + 1e-3f, 1) == 0.0f);
}

TEST_CASE("the correction decays to exactly zero at the grid's latitude edges", "[render][parallax]") {
    constexpr std::uint32_t W = 512, H = 40, mapH = 256;
    const auto flat = [](std::uint32_t x, std::uint32_t y) { return bandTexture(x, y, W); };
    const render::LensBands bands = makeBands(W, H, mapH, flat, flat);
    const render::BidirFlow flow = uniformFlow(W, H, 2.0f, -3.0f);

    render::ParallaxWarpParams p;
    p.gridW = 64;
    p.gridRows = 8;
    p.decayRows = 6;
    p.requiredImprovement = 0.0;

    SECTION("with the ring: zero at the edge, full inside, monotone between") {
        auto grid = render::gridFromFlow(bands, flow, p);
        REQUIRE(grid.ok());
        const render::ParallaxWarpGrid& g = grid.value();
        // Exactly zero, not merely small: the kernel returns 0 for any ray
        // beyond these rows, and anything non-zero here would be a step.
        REQUIRE(rowMaxAbs(g, 0) == 0.0);
        REQUIRE(rowMaxAbs(g, g.h - 1) == 0.0);
        const double full = rowMaxAbs(g, p.decayRows);
        REQUIRE(full > 0.0);
        // Strictly rising toward the measured rows from both ends, and
        // symmetric: the ring is a ramp, not a cliff.
        for (std::uint32_t j = 0; j < p.decayRows; ++j) {
            const double top = rowMaxAbs(g, j);
            const double topNext = rowMaxAbs(g, j + 1);
            const double bottom = rowMaxAbs(g, g.h - 1 - j);
            REQUIRE(top < topNext);
            REQUIRE(std::fabs(top - bottom) < 1e-9);
        }
        // The largest row-to-row step in the ring is a fraction of the full
        // value - a smooth fall-off rather than a jump in its last row.
        double maxStep = 0.0;
        for (std::uint32_t j = 0; j < p.decayRows; ++j) {
            maxStep = std::max(maxStep, rowMaxAbs(g, j + 1) - rowMaxAbs(g, j));
        }
        REQUIRE(maxStep < 0.5 * full);
    }

    SECTION("without the ring the edge rows carry the full correction (the torn edge the ring prevents)") {
        p.decayRows = 0;
        auto grid = render::gridFromFlow(bands, flow, p);
        REQUIRE(grid.ok());
        const render::ParallaxWarpGrid& g = grid.value();
        REQUIRE(g.h == p.gridRows);
        REQUIRE(std::fabs(rowMaxAbs(g, 0) - rowMaxAbs(g, g.h / 2)) < 1e-9);
    }
}

TEST_CASE("zero flow gives an all-zero grid", "[render][parallax]") {
    constexpr std::uint32_t W = 256, H = 32, mapH = 128;
    const auto flat = [](std::uint32_t x, std::uint32_t y) { return bandTexture(x, y, W); };
    const render::LensBands bands = makeBands(W, H, mapH, flat, flat);
    const render::BidirFlow flow = uniformFlow(W, H, 0.0f, 0.0f);
    render::ParallaxWarpParams p;
    p.gridW = 32;
    p.gridRows = 8;
    for (double gate : {0.0, 0.2}) {
        p.requiredImprovement = gate;
        auto grid = render::gridFromFlow(bands, flow, p);
        REQUIRE(grid.ok());
        for (float v : grid.value().uv) {
            REQUIRE(v == 0.0f);
        }
        REQUIRE(grid.value().maxAbsCorrectionDeg == 0.0);
    }
}

TEST_CASE("the benefit gate keeps a correction that helps and drops one that does not", "[render][parallax]") {
    constexpr std::uint32_t W = 512, H = 40, mapH = 256;
    constexpr float fu = 2.0f, fv = -3.0f;
    const auto a = [](std::uint32_t x, std::uint32_t y) { return bandTexture(x, y, W); };
    // Master band consistent with the forward flow: a(p) = b(p + f), so
    // b(q) = a(q - f).
    const auto bTrue = [](std::uint32_t x, std::uint32_t y) {
        return bandTexture(static_cast<double>(x) - fu, static_cast<double>(y) - fv, W);
    };
    // A master band showing something else entirely - the two lenses seeing
    // DIFFERENT objects, as at the sample clip's wingtip light.  Hashed noise
    // rather than another sinusoid: a smooth periodic texture can line up
    // with the first one under some shift by coincidence, which would test
    // the texture, not the gate.  Independent noise has no such shift.
    const auto bOther = [](std::uint32_t x, std::uint32_t y) {
        std::uint32_t hsh = x * 73856093u ^ y * 19349663u ^ 0x9E3779B9u;
        hsh ^= hsh >> 13;
        hsh *= 0x5bd1e995u;
        hsh ^= hsh >> 15;
        return 0.2 + 0.6 * static_cast<double>(hsh & 0xFFFFu) / 65535.0;
    };
    const render::BidirFlow flow = uniformFlow(W, H, fu, fv);
    render::ParallaxWarpParams p;
    p.gridW = 64;
    p.gridRows = 8;
    p.decayRows = 4;
    p.requiredImprovement = 0.2;

    SECTION("consistent content: nothing is gated and the full correction survives") {
        auto grid = render::gridFromFlow(makeBands(W, H, mapH, a, bTrue), flow, p);
        REQUIRE(grid.ok());
        const render::ParallaxWarpGrid& g = grid.value();
        REQUIRE(g.measuredCells == g.w * p.gridRows);
        REQUIRE(g.gatedCells == 0);
        const double expectLat = 0.5 * 3.0 * (osv::kPi / mapH);
        REQUIRE(std::fabs(cell(g, 10, p.decayRows + 3, 1) - expectLat) < 1e-3 * expectLat);
    }

    SECTION("different content: the gate refuses to warp for no gain") {
        auto grid = render::gridFromFlow(makeBands(W, H, mapH, a, bOther), flow, p);
        REQUIRE(grid.ok());
        const render::ParallaxWarpGrid& g = grid.value();
        // Judge the AMOUNT of warp that survives, not the count of cells at
        // exactly zero.  Over unrelated content the per-cell residual ratio
        // scatters around 1 (a cell averages ~45 pixels), so a minority of
        // cells land a little below 1 and keep a small smoothstep weight;
        // counting only the hard zeros would misrepresent that.
        const double fullLat = 0.5 * 3.0 * (osv::kPi / mapH);
        double sumLat = 0.0;
        double maxLat = 0.0;
        std::size_t n = 0;
        for (std::uint32_t y = p.decayRows; y < p.decayRows + p.gridRows; ++y) {
            for (std::uint32_t x = 0; x < g.w; ++x) {
                const double v = std::fabs(static_cast<double>(cell(g, x, y, 1)));
                sumLat += v;
                maxLat = std::max(maxLat, v);
                ++n;
            }
        }
        const double meanFrac = sumLat / static_cast<double>(n) / fullLat;
        INFO("gated " << g.gatedCells << " of " << g.measuredCells << "; surviving warp mean " << meanFrac
                      << " / max " << maxLat / fullLat << " of the full correction");
        // Measured: 422 of 512 cells hard-zeroed, surviving warp 1.9% of the
        // full correction on average and 21% at worst.  Before the gate used
        // a fair null and pooled neighbours, the same input let 49% through
        // on average - which is exactly what these bounds exist to catch.
        REQUIRE(g.gatedCells * 4 > g.measuredCells * 3);
        REQUIRE(meanFrac < 0.1);
        REQUIRE(maxLat / fullLat < 0.4);
    }
}

// ===========================================================================
//  End to end: a known 2-D parallax through the real rig geometry
// ===========================================================================

TEST_CASE("a known 2-D parallax is measured and removed from the overlap", "[render][parallax]") {
    const SceneFixture& s = scene();
    REQUIRE(s.ok);
    ThreadPool pool;
    geom::BlendParams blend;
    const render::ParallaxWarpParams params = syntheticParams();

    auto grid = render::buildParallaxWarp(s.rig, s.parallax, blend, params, nullptr, pool);
    REQUIRE(grid.ok());
    const render::ParallaxWarpGrid& g = grid.value();
    REQUIRE(g.usedBackend == render::FlowBackendKind::Classical);
    INFO("consistent " << g.consistentFraction() << ", gated " << g.gatedCells << "/" << g.measuredCells);

    // The grid carries HALF the disparity for the master: +dLon/2, +dLat/2.
    std::vector<double> lon, lat;
    for (std::uint32_t y = params.decayRows + 2; y + 2 < params.decayRows + params.gridRows; ++y) {
        for (std::uint32_t x = 0; x < g.w; ++x) {
            lon.push_back(rad2deg(cell(g, x, y, 0)));
            lat.push_back(rad2deg(cell(g, x, y, 1)));
        }
    }
    const double medLon = median(lon);
    const double medLat = median(lat);
    INFO("median half disparity: lon " << medLon << " deg (expect " << kParallaxLonDeg / 2 << "), lat " << medLat
                                       << " deg (expect " << kParallaxLatDeg / 2 << ")");
    REQUIRE(std::fabs(medLat - kParallaxLatDeg / 2) < 0.2 * kParallaxLatDeg / 2);
    REQUIRE(std::fabs(medLon - kParallaxLonDeg / 2) < 0.3 * kParallaxLonDeg / 2);

    // And it actually closes the gap: the two lenses agree across the
    // overlap once the kernel applies the grid.
    const render::BandParams scored = syntheticBand(4.0);
    auto before = render::overlapNcc(s.rig, s.parallax, blend, scored, pool);
    const render::WarpGridView view = viewOf(g);
    auto after = render::overlapNcc(s.rig, s.parallax, blend, scored, pool, nullptr, &view);
    REQUIRE(before.ok());
    REQUIRE(after.ok());
    INFO("overlap NCC before " << before.value() << ", after " << after.value());
    REQUIRE(after.value() > before.value() + 0.2);
    REQUIRE(after.value() > 0.9);
}

TEST_CASE("no parallax, no correction: a flat pair yields a near-zero grid", "[render][parallax]") {
    const SceneFixture& s = scene();
    REQUIRE(s.ok);
    ThreadPool pool;
    geom::BlendParams blend;
    auto grid = render::buildParallaxWarp(s.rig, s.flat, blend, syntheticParams(), nullptr, pool);
    REQUIRE(grid.ok());
    // The rig's two axes are not perfectly antiparallel, but the scene is
    // defined in the body frame, so the lenses agree exactly: whatever the
    // flow reports is solver noise, far below a tenth of a band pixel.
    const double tenthPixelDeg = 0.1 * 180.0 / 512.0;
    INFO("max full correction " << grid.value().maxAbsCorrectionDeg << " deg");
    REQUIRE(grid.value().maxAbsCorrectionDeg < tenthPixelDeg);
}

// ===========================================================================
//  Defensive behaviour
// ===========================================================================

TEST_CASE("gridFromFlow rejects malformed input instead of guessing", "[render][parallax]") {
    constexpr std::uint32_t W = 256, H = 32, mapH = 128;
    const auto flat = [](std::uint32_t x, std::uint32_t y) { return bandTexture(x, y, W); };
    const render::LensBands bands = makeBands(W, H, mapH, flat, flat);
    const render::BidirFlow flow = uniformFlow(W, H, 1.0f, 1.0f);
    render::ParallaxWarpParams p;
    p.gridW = 32;
    p.gridRows = 8;
    REQUIRE(render::gridFromFlow(bands, flow, p).ok());

    const auto rejects = [](const Result<render::ParallaxWarpGrid>& r) {
        return !r.ok() && r.error().code == ErrorCode::InvalidArgument;
    };

    SECTION("empty band") { REQUIRE(rejects(render::gridFromFlow(render::LensBands{}, flow, p))); }
    SECTION("flow of another size") { REQUIRE(rejects(render::gridFromFlow(bands, uniformFlow(10, 10, 0, 0), p))); }
    SECTION("mask of the wrong size") {
        render::BidirFlow bad = flow;
        bad.ok.pop_back();
        REQUIRE(rejects(render::gridFromFlow(bands, bad, p)));
    }
    SECTION("empty flow") { REQUIRE(rejects(render::gridFromFlow(bands, render::BidirFlow{}, p))); }
    SECTION("coverage planes of the wrong size") {
        render::LensBands bad = bands;
        bad.alpha[1].resize(3);
        REQUIRE(rejects(render::gridFromFlow(bad, flow, p)));
    }
    SECTION("band outside its own map") {
        render::LensBands bad = bands;
        bad.rowOffset = mapH;  // + h > mapH
        REQUIRE(rejects(render::gridFromFlow(bad, flow, p)));
    }
    SECTION("parameters outside their documented ranges") {
        render::ParallaxWarpParams q = p;
        q.gridW = 4;
        REQUIRE(rejects(render::gridFromFlow(bands, flow, q)));
        q = p;
        q.gridRows = 2;
        REQUIRE(rejects(render::gridFromFlow(bands, flow, q)));
        q = p;
        q.crossMeridianScale = 1.5;
        REQUIRE(rejects(render::gridFromFlow(bands, flow, q)));
        q = p;
        q.requiredImprovement = 1.0;
        REQUIRE(rejects(render::gridFromFlow(bands, flow, q)));
        q = p;
        q.maxCorrectionDeg = 0.0;
        REQUIRE(rejects(render::gridFromFlow(bands, flow, q)));
        q = p;
        q.crossMeridianSmooth = std::numeric_limits<double>::quiet_NaN();
        REQUIRE(rejects(render::gridFromFlow(bands, flow, q)));
        q = p;
        q.minConsistentFraction = -0.1;
        REQUIRE(rejects(render::gridFromFlow(bands, flow, q)));
    }
}

TEST_CASE("buildParallaxWarp validates its parameters before doing any work", "[render][parallax]") {
    ThreadPool pool;
    render::ParallaxWarpParams p;
    p.gridW = 0;
    // An empty rig and frame pair: the parameter check must fire first, so
    // this returns InvalidArgument rather than crashing in the band render.
    auto r = render::buildParallaxWarp(geom::LensRig{}, video::FramePair{}, geom::BlendParams{}, p, nullptr, pool);
    REQUIRE(!r.ok());
    REQUIRE(r.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("RenderParamsBuilder refuses a half-configured warp grid", "[render][parallax]") {
    const SceneFixture& s = scene();
    REQUIRE(s.ok);
    const OsvColorParams cp =
        color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Rec709, 0.0f);
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::Standard;
    map.w = 64;
    map.h = 32;
    const float top = static_cast<float>(deg2rad(8.0));
    const float bottom = static_cast<float>(deg2rad(-8.0));
    const auto paramsWith = [&](const std::vector<float>& uv, std::uint32_t w, std::uint32_t h, float a, float b) {
        render::RenderParamsBuilder builder;
        builder.rig(s.rig).equirect(map).color(cp).warp(uv, w, h, a, b);
        return builder.build(s.flat);
    };

    // Payload does not match the declared size: disabled, not half-set.
    auto mismatched = paramsWith(std::vector<float>(10, 0.1f), 8, 4, top, bottom);
    REQUIRE(mismatched.ok());
    REQUIRE(mismatched.value().params.warpEnabled == 0);
    REQUIRE(mismatched.value().warpGrid.empty());

    // A degenerate latitude span would make the kernel divide by ~0.
    auto degenerate = paramsWith(std::vector<float>(8u * 4u * 2u, 0.1f), 8, 4, top, top);
    REQUIRE(degenerate.ok());
    REQUIRE(degenerate.value().params.warpEnabled == 0);

    // Non-finite latitudes are rejected the same way.
    auto nonFinite = paramsWith(std::vector<float>(8u * 4u * 2u, 0.1f), 8, 4,
                                std::numeric_limits<float>::quiet_NaN(), bottom);
    REQUIRE(nonFinite.ok());
    REQUIRE(nonFinite.value().params.warpEnabled == 0);

    // A well-formed grid is carried through intact...
    auto good = paramsWith(std::vector<float>(8u * 4u * 2u, 0.1f), 8, 4, top, bottom);
    REQUIRE(good.ok());
    REQUIRE(good.value().params.warpEnabled == 1);
    REQUIRE(good.value().params.warpW == 8);
    REQUIRE(good.value().params.warpH == 4);
    REQUIRE(good.value().warpGrid.size() == 8u * 4u * 2u);
    REQUIRE(good.value().valid());

    // ...and a job whose grid no longer matches its parameters is invalid,
    // so no renderer will index past the end of it.
    render::RenderJob tampered = good.value();
    tampered.warpGrid.pop_back();
    REQUIRE(!tampered.valid());

    // clearWarp() really clears.
    render::RenderParamsBuilder builder;
    builder.rig(s.rig).equirect(map).color(cp).warp(std::vector<float>(8u * 4u * 2u, 0.1f), 8, 4, top, bottom);
    builder.clearWarp();
    auto cleared = builder.build(s.flat);
    REQUIRE(cleared.ok());
    REQUIRE(cleared.value().params.warpEnabled == 0);

    // The parameter block still fits the budget the other tests assert.
    REQUIRE(sizeof(OsvRenderParams) <= 4096);
}

// ===========================================================================
//  GPU backends: the warp must be applied exactly as the CPU reference does
// ===========================================================================

namespace {

/// A measured grid for the synthetic parallax pair (built once).
const render::ParallaxWarpGrid& syntheticGrid() {
    static const render::ParallaxWarpGrid grid = [] {
        const SceneFixture& s = scene();
        ThreadPool pool;
        auto g = render::buildParallaxWarp(s.rig, s.parallax, geom::BlendParams{}, syntheticParams(), nullptr, pool);
        return g.ok() ? std::move(g).value() : render::ParallaxWarpGrid{};
    }();
    return grid;
}

/// Jobs that exercise the warp: a full equirect (every seam direction) and
/// a rectilinear view aimed straight at the seam, where the correction is
/// largest on screen.
std::vector<render::RenderJob> warpJobs() {
    const SceneFixture& s = scene();
    const render::ParallaxWarpGrid& g = syntheticGrid();
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    std::vector<render::RenderJob> jobs;

    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::Standard;
    map.w = 2048;
    map.h = 1024;
    auto a = render::RenderParamsBuilder()
                 .rig(s.rig)
                 .equirect(map)
                 .color(cp)
                 .warp(g.uv, g.w, g.h, g.latMinRad, g.latMaxRad)
                 .build(s.parallax);
    if (a.ok()) {
        jobs.push_back(std::move(a).value());
    }

    geom::VirtualCamera cam;
    cam.w = 1280;
    cam.h = 720;
    cam.hfovDeg = 100;
    cam.yawDeg = 90;  // perpendicular to both lens axes: the seam runs through the middle
    auto b = render::RenderParamsBuilder()
                 .rig(s.rig)
                 .camera(cam)
                 .color(cp)
                 .warp(g.uv, g.w, g.h, g.latMinRad, g.latMaxRad)
                 .build(s.parallax);
    if (b.ok()) {
        jobs.push_back(std::move(b).value());
    }
    return jobs;
}

void checkWarpParity(render::IRenderer& gpu, const char* label) {
    REQUIRE(scene().ok);
    REQUIRE(syntheticGrid().valid());
    ThreadPool pool;
    render::CpuRenderer cpu(pool);
    const std::vector<render::RenderJob> jobs = warpJobs();
    REQUIRE(jobs.size() == 2);
    for (const render::RenderJob& job : jobs) {
        REQUIRE(job.params.warpEnabled == 1);
        auto ref = cpu.render(job);
        auto test = gpu.render(job);
        REQUIRE(ref.ok());
        REQUIRE(test.ok());
        const render::ImageDiffStats stats = render::compareImages16(ref.value(), test.value());
        INFO(label << ": PSNR " << stats.psnrDb << " dB, max diff " << stats.maxAbsCode << " codes, within2 "
                   << stats.fractionWithin2);
        // The same bar as the unwarped parity tests in test_render.cpp.
        REQUIRE(stats.psnrDb >= 60.0);
        REQUIRE(stats.fractionWithin2 >= 0.9995);
        REQUIRE(stats.maxAbsCode <= 64);
    }
}

}  // namespace

#if defined(OSV_HAVE_CUDA)
TEST_CASE("CUDA applies the warp grid exactly as the CPU reference does", "[render][parallax][cuda]") {
    std::string reason;
    if (!render::CudaRenderer::available(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    auto r = render::CudaRenderer::create(0);
    REQUIRE(r.ok());
    checkWarpParity(*r.value(), "cuda");
}

TEST_CASE("cost of the warp in the CUDA kernel", "[render][parallax][cuda][!benchmark]") {
    std::string reason;
    if (!render::CudaRenderer::available(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    auto r = render::CudaRenderer::create(0);
    REQUIRE(r.ok());
    const SceneFixture& s = scene();
    REQUIRE(s.ok);
    const render::ParallaxWarpGrid& g = syntheticGrid();
    REQUIRE(g.valid());
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);

    // The importer's native output: a 6000 x 3000 Standard equirect.
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::Standard;
    map.w = 6000;
    map.h = 3000;
    auto plain = render::RenderParamsBuilder().rig(s.rig).equirect(map).color(cp).build(s.parallax);
    auto warped = render::RenderParamsBuilder()
                      .rig(s.rig)
                      .equirect(map)
                      .color(cp)
                      .warp(g.uv, g.w, g.h, g.latMinRad, g.latMaxRad)
                      .build(s.parallax);
    REQUIRE(plain.ok());
    REQUIRE(warped.ok());

    // Reported, not asserted.  Each figure includes the upload of the inputs
    // and the download of the 288 MB float result, which are identical for
    // both jobs, so the DIFFERENCE is the kernel's warp cost.
    const auto timeJob = [&](const render::RenderJob& job) {
        REQUIRE(r.value()->render(job).ok());  // warm-up
        constexpr int kRuns = 8;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kRuns; ++i) {
            REQUIRE(r.value()->render(job).ok());
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kRuns;
    };
    const double msPlain = timeJob(plain.value());
    const double msWarped = timeJob(warped.value());
    WARN("CUDA 6000 x 3000 equirect: " << msPlain << " ms plain, " << msWarped << " ms with the warp grid ("
                                        << (msWarped - msPlain) << " ms for the warp)");

    // The CPU backend, for machines without a GPU: here per-pixel arithmetic
    // is the whole cost, so the warp's share is directly visible.
    ThreadPool pool;
    render::CpuRenderer cpu(pool);
    const auto timeCpu = [&](const render::RenderJob& job) {
        REQUIRE(cpu.render(job).ok());
        constexpr int kRuns = 3;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kRuns; ++i) {
            REQUIRE(cpu.render(job).ok());
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kRuns;
    };
    const double cpuPlain = timeCpu(plain.value());
    const double cpuWarped = timeCpu(warped.value());
    WARN("CPU 6000 x 3000 equirect: " << cpuPlain << " ms plain, " << cpuWarped << " ms with the warp grid ("
                                       << (cpuWarped - cpuPlain) << " ms for the warp)");
}
#endif

#if defined(OSV_HAVE_OPENCL)
TEST_CASE("OpenCL applies the warp grid exactly as the CPU reference does", "[render][parallax][opencl]") {
    std::string reason;
    if (!render::OpenClRenderer::available(&reason)) {
        SKIP("OpenCL unavailable: " << reason);
    }
    auto r = render::OpenClRenderer::create(0);
    if (!r.ok()) {
        FAIL("OpenCL renderer creation failed: " << r.error().message);
    }
    checkWarpParity(*r.value(), "opencl");
}
#endif

// ===========================================================================
//  The sample clip: the hard case
// ===========================================================================

namespace {

struct Loaded {
    std::unique_ptr<OsvFile> file;
    meta::MetadataTrack track;
    meta::FormatInfo format;
    meta::CalibrationSet cal;
    video::FramePair pair;
};

Result<Loaded> loadSample(std::uint32_t frame) {
    Loaded l;
    OSV_TRY_ASSIGN(OsvFile f, OsvFile::open(osvtest::sampleOsv()));
    l.file = std::make_unique<OsvFile>(std::move(f));
    OSV_TRY_ASSIGN(l.track, meta::MetadataTrack::load(*l.file));
    OSV_TRY_ASSIGN(l.format, meta::FormatDetector::detect(*l.file, &l.track));
    OSV_TRY_ASSIGN(l.cal, meta::CalibrationSelector::select(l.track.stream()));
    OSV_TRY_ASSIGN(video::DualStreamReader reader, video::DualStreamReader::open(osvtest::sampleOsv(), l.format));
    OSV_TRY_ASSIGN(l.pair, reader.read(frame));
    return l;
}

Result<geom::LensRig> sampleRig(const Loaded& l) {
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(static_cast<int>(l.format.streamW), static_cast<int>(l.format.streamH),
                                               static_cast<int>(l.format.sensorW), static_cast<int>(l.format.sensorH),
                                               l.format.digitalFocalLength, 0.5 * (l.cal.slave.fx + l.cal.master.fx),
                                               std::nullopt));
    return geom::LensRig::build(l.cal, scaling, geom::FocalSource::DigitalFocalLength, l.format.digitalFocalLength,
                                geom::ExtrinsicConvention{});
}

}  // namespace

TEST_CASE("on the sample clip the correction improves the overlap and does no harm at the wingtip",
          "[render][parallax][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto l = loadSample(0);
    REQUIRE(l.ok());
    auto rig = sampleRig(l.value());
    REQUIRE(rig.ok());
    ThreadPool pool;
    geom::BlendParams blend;

    render::ParallaxWarpParams params;
    params.backend = render::FlowBackendKind::Classical;
    auto grid = render::buildParallaxWarp(rig.value(), l.value().pair, blend, params, nullptr, pool);
    REQUIRE(grid.ok());
    const render::WarpGridView view = viewOf(grid.value());

    // Whole band, scored exactly as `osvtool seam` does.  Measured 0.825 ->
    // 0.918 on frame 0; the 1-D seam search reaches 0.897.
    render::BandParams band;
    band.bandHalfDeg = 4.0;
    auto before = render::overlapNcc(rig.value(), l.value().pair, blend, band, pool);
    auto after = render::overlapNcc(rig.value(), l.value().pair, blend, band, pool, nullptr, &view);
    REQUIRE(before.ok());
    REQUIRE(after.ok());
    INFO("whole-band NCC before " << before.value() << ", after " << after.value());
    REQUIRE(after.value() >= before.value() + 0.05);
    REQUIRE(after.value() >= 0.90);

    auto plainBands = render::renderLensBands(rig.value(), l.value().pair, blend, band, false, nullptr, pool);
    auto warpBands = render::renderLensBands(rig.value(), l.value().pair, blend, band, false, nullptr, pool, &view);
    REQUIRE(plainBands.ok());
    REQUIRE(warpBands.ok());

    // Textured ground crossing the seam (band columns 1100-1900): the
    // double image the user sees.  Measured 0.397 -> 0.896.
    const double groundBefore = regionNcc(plainBands.value(), 1100, 1900);
    const double groundAfter = regionNcc(warpBands.value(), 1100, 1900);
    INFO("ground NCC before " << groundBefore << ", after " << groundAfter);
    REQUIRE(groundAfter >= groundBefore + 0.3);

    // The wingtip (columns 1940-2040): a light cover a few centimetres from
    // one lens that the other lens cannot see at all.  No warp can align
    // two different objects; what matters is that the correction does not
    // make it WORSE, which it did (0.32 -> 0.28) before the benefit gate.
    const double wingBefore = regionNcc(plainBands.value(), 1940, 2040);
    const double wingAfter = regionNcc(warpBands.value(), 1940, 2040);
    INFO("wingtip NCC before " << wingBefore << ", after " << wingAfter);
    REQUIRE(wingAfter >= wingBefore - 0.02);
}
