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

#include <catch2/catch_approx.hpp>
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

TEST_CASE("a parallax warp never makes a direction the rig sees transparent", "[render][parallax][coverage]") {
    // The warp closes a disparity by moving each lens's sample toward its
    // own rim, which lowers both feather weights.  Those weights must still
    // decide the MIX, but not the coverage: the direction is exactly as seen
    // as it was.  Taking the alpha from the warped weights made every warped
    // seam partly transparent in proportion to the parallax it corrected -
    // on a camera set on the ground, dark cones along the seam, one per row
    // of the grid, wherever the host showed black through.
    const SceneFixture& s = scene();
    REQUIRE(s.ok);
    ThreadPool pool;
    render::CpuRenderer cpu(pool);
    geom::BlendParams blend;
    const OsvColorParams cp =
        color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Rec709, 0.0f);
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::Standard;
    map.w = 512;
    map.h = 256;

    render::RenderParamsBuilder plainBuilder;
    plainBuilder.rig(s.rig).equirect(map).blend(blend, true).color(cp).alphaCoverage(true);
    auto plainJob = plainBuilder.build(s.parallax);
    REQUIRE(plainJob.ok());
    auto plain = cpu.render(plainJob.value());
    REQUIRE(plain.ok());

    // A constant 9 degree correction, both ways across the meridian and
    // along it: whichever way the seam runs, one of them pushes both lenses
    // outward, and an object close to the camera asks for that much.  Before
    // the coverage came from the unwarped rays, 6 degrees took up to 42 % of
    // the alpha from 12,349 of these pixels and 9 degrees all of it.
    const float step = static_cast<float>(deg2rad(9.0));
    for (const std::pair<float, float> d : {std::pair<float, float>{step, 0.0f}, {-step, 0.0f}, {0.0f, step},
                                            {0.0f, -step}}) {
        std::vector<float> uv(32u * 8u * 2u);
        for (std::size_t i = 0; i < uv.size(); i += 2) {
            uv[i] = d.first;
            uv[i + 1] = d.second;
        }
        render::RenderParamsBuilder warpBuilder;
        warpBuilder.rig(s.rig).equirect(map).blend(blend, true).color(cp).alphaCoverage(true).warp(
            uv, 32, 8, static_cast<float>(deg2rad(80.0)), static_cast<float>(deg2rad(-80.0)));
        auto warpJob = warpBuilder.build(s.parallax);
        REQUIRE(warpJob.ok());
        REQUIRE(warpJob.value().params.warpEnabled == 1);
        auto warped = cpu.render(warpJob.value());
        REQUIRE(warped.ok());
        REQUIRE(warped.value().data.size() == plain.value().data.size());

        // Alpha may only ever rise where the warp found a lens the plain
        // blend did not use; it may never fall.
        std::size_t darker = 0;
        float worst = 0.0f;
        const std::vector<float>& a = plain.value().data;
        const std::vector<float>& b = warped.value().data;
        for (std::size_t i = 3; i < a.size(); i += 4) {
            const float drop = a[i] - b[i];
            if (drop > 1e-5f) {
                ++darker;
                worst = std::max(worst, drop);
            }
        }
        INFO("warp (" << d.first << ", " << d.second << "): " << darker << " pixels lost coverage, worst by "
                      << worst);
        CHECK(darker == 0u);
    }
}

TEST_CASE("the per-column seam table acts only near the seam", "[render][parallax][seamtable]") {
    // The table is measured in a band of +/- 6 degrees around the seam and
    // says nothing about the rest of the sphere.  Applied to every ray, it
    // rotated each one toward or away from its lens axis - a real 8K clip
    // with a table of -3 degrees in places bent a fence thirty metres away
    // into a staircase.  It now acts in full within 6 degrees of the seam,
    // fades out by 12, and leaves everything beyond bit for bit alone.
    const SceneFixture& s = scene();
    REQUIRE(s.ok);
    ThreadPool pool;
    render::CpuRenderer cpu(pool);
    geom::BlendParams blend;
    const OsvColorParams cp =
        color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Rec709, 0.0f);

    // The polar-axis layout: row r is latitude 90 - 180 (r + 0.5) / H from
    // the seam plane, so the seam is the middle row.
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::PolarAxis;
    map.w = 512;
    map.h = 256;

    render::RenderParamsBuilder plainBuilder;
    plainBuilder.rig(s.rig).equirect(map).blend(blend, true).color(cp);
    auto plainJob = plainBuilder.build(s.parallax);
    REQUIRE(plainJob.ok());
    auto plain = cpu.render(plainJob.value());
    REQUIRE(plain.ok());

    // A constant 3 degree disparity for every column.
    render::RenderParamsBuilder tableBuilder;
    tableBuilder.rig(s.rig).equirect(map).blend(blend, true).color(cp).seam(std::vector<float>(2048u, 3.0f));
    auto tableJob = tableBuilder.build(s.parallax);
    REQUIRE(tableJob.ok());
    REQUIRE(tableJob.value().params.seamShiftEnabled == 1);
    auto table = cpu.render(tableJob.value());
    REQUIRE(table.ok());

    const std::vector<float>& a = plain.value().data;
    const std::vector<float>& b = table.value().data;
    REQUIRE(a.size() == b.size());
    std::size_t farChanged = 0;
    std::size_t nearChanged = 0;
    std::size_t nearTotal = 0;
    for (std::uint32_t y = 0; y < 256u; ++y) {
        const double latDeg = 90.0 - 180.0 * (static_cast<double>(y) + 0.5) / 256.0;
        for (std::uint32_t x = 0; x < 512u; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * 512u + x) * 4u;
            const bool differs = std::memcmp(&a[i], &b[i], 4u * sizeof(float)) != 0;
            if (std::fabs(latDeg) > 12.5) {
                farChanged += differs ? 1u : 0u;
            } else if (std::fabs(latDeg) < 4.0) {
                ++nearTotal;
                nearChanged += differs ? 1u : 0u;
            }
        }
    }
    INFO("far pixels changed: " << farChanged << "; near pixels changed: " << nearChanged << " of " << nearTotal);
    // Nothing beyond the fade moves...
    CHECK(farChanged == 0u);
    // ...and the table still does its job at the seam.
    CHECK(nearChanged > nearTotal / 2u);
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

TEST_CASE("gridFromFlow is bit-identical with and without a pool", "[render][parallax]") {
    // The importer builds grids on its render thread WITH a pool and in its
    // background worker WITHOUT one, and an export must not depend on which
    // got there first - the same guarantee DisFlow.cpp gives for the flow.
    // So this compares with ==, on input chosen to exercise every branch of
    // both per-pixel passes: spatially varying flow, pixels that failed the
    // consistency check, pixels only one lens covers, a band height that
    // does not divide evenly into grid rows, and a gate that keeps some
    // cells and drops others.
    constexpr std::uint32_t W = 700, H = 53, mapH = 400;
    const auto a = [](std::uint32_t x, std::uint32_t y) { return bandTexture(x, y, W); };
    const auto b = [](std::uint32_t x, std::uint32_t y) {
        // The right half matches a shifted copy; the left half is unrelated.
        if (x >= W / 2) {
            return bandTexture(static_cast<double>(x) - 1.5, static_cast<double>(y) + 0.5, W);
        }
        return bandTexture(static_cast<double>(x) * 1.7 + 11.0, static_cast<double>(y) * 0.6, W);
    };
    render::LensBands bands = makeBands(W, H, mapH, a, b);
    render::BidirFlow flow = uniformFlow(W, H, 1.5f, -0.5f);
    for (std::uint32_t y = 0; y < H; ++y) {
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * W + x;
            // Motion that varies across the band so cells disagree.
            flow.forward.u[i] += 0.8f * static_cast<float>(std::sin(0.02 * x + 0.1 * y));
            flow.backward.u[i] -= 0.8f * static_cast<float>(std::sin(0.02 * x + 0.1 * y));
            // Every 7th pixel inconsistent, every 11th covered by one lens.
            if (i % 7 == 0) {
                flow.ok[i] = 0u;
            }
            if (i % 11 == 0) {
                bands.alpha[1][i] = 0.0f;
            }
        }
    }
    render::ParallaxWarpParams p;
    p.gridW = 96;
    p.gridRows = 12;
    p.decayRows = 3;

    const auto sequential = render::gridFromFlow(bands, flow, p, nullptr);
    REQUIRE(sequential.ok());
    REQUIRE(sequential.value().measuredCells > 0);
    for (const unsigned threads : {2u, 5u, 32u}) {
        ThreadPool pool(threads);
        const auto pooled = render::gridFromFlow(bands, flow, p, &pool);
        REQUIRE(pooled.ok());
        INFO("pool of " << threads << " threads");
        const render::ParallaxWarpGrid& s = sequential.value();
        const render::ParallaxWarpGrid& q = pooled.value();
        CHECK(q.uv == s.uv);
        CHECK(q.consistentPixels == s.consistentPixels);
        CHECK(q.totalPixels == s.totalPixels);
        CHECK(q.measuredCells == s.measuredCells);
        CHECK(q.gatedCells == s.gatedCells);
        CHECK(q.meanAbsCorrectionDeg == s.meanAbsCorrectionDeg);
        CHECK(q.maxAbsCorrectionDeg == s.maxAbsCorrectionDeg);
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

// =============================================================================
//  Temporal schedule: one measurement per bucket, blended across bucket edges
// =============================================================================

namespace {

/// A valid grid whose every correction component is `value`.
render::ParallaxWarpGrid constantGrid(float value, std::uint32_t w = 16, std::uint32_t h = 6) {
    render::ParallaxWarpGrid g;
    g.w = w;
    g.h = h;
    g.latMinRad = -0.1f;
    g.latMaxRad = 0.1f;
    g.uv.assign(static_cast<std::size_t>(w) * h * 2u, value);
    return g;
}

}  // namespace

TEST_CASE("parallaxBucket groups kParallaxBucketFrames frames per measurement", "[parallax][schedule]") {
    const std::uint32_t n = render::kParallaxBucketFrames;
    REQUIRE(n >= 2);
    for (std::uint32_t f = 0; f < n; ++f) {
        CHECK(render::parallaxBucket(f) == 0u);
    }
    CHECK(render::parallaxBucket(n) == 1u);
    CHECK(render::parallaxBucket(2 * n - 1) == 1u);
    CHECK(render::parallaxBucket(2 * n) == 2u);

    SECTION("a zero bucket size means every frame is its own bucket, not a division by zero") {
        CHECK(render::parallaxBucket(0, 0) == 0u);
        CHECK(render::parallaxBucket(17, 0) == 17u);
    }
}

TEST_CASE("parallaxCrossfadeWeight reaches the own grid exactly at the bucket's last frame", "[parallax][schedule]") {
    const std::uint32_t n = render::kParallaxBucketFrames;
    // First frame of a bucket moves only 1/N of the way to the new grid; the
    // last frame is the new grid alone.
    CHECK(render::parallaxCrossfadeWeight(0) == 1.0 / n);
    CHECK(render::parallaxCrossfadeWeight(n - 1) == 1.0);
    CHECK(render::parallaxCrossfadeWeight(n) == 1.0 / n);
    // Strictly increasing within a bucket, always in (0, 1].
    for (std::uint32_t f = 1; f < n; ++f) {
        CHECK(render::parallaxCrossfadeWeight(f) > render::parallaxCrossfadeWeight(f - 1));
    }
    for (std::uint32_t f = 0; f < 4 * n; ++f) {
        const double w = render::parallaxCrossfadeWeight(f);
        CHECK(w > 0.0);
        CHECK(w <= 1.0);
    }
    SECTION("a bucket of one frame (or none) never blends") {
        CHECK(render::parallaxCrossfadeWeight(5, 1) == 1.0);
        CHECK(render::parallaxCrossfadeWeight(5, 0) == 1.0);
    }
}

TEST_CASE("blendParallaxGrids interpolates the correction and keeps the newer diagnostics", "[parallax][schedule]") {
    render::ParallaxWarpGrid from = constantGrid(0.0f);
    render::ParallaxWarpGrid to = constantGrid(0.8f);
    to.gatedCells = 42;  // a diagnostic that must come from `to`

    const auto at0 = render::blendParallaxGrids(from, to, 0.0);
    const auto at1 = render::blendParallaxGrids(from, to, 1.0);
    const auto half = render::blendParallaxGrids(from, to, 0.5);
    REQUIRE(at0.ok());
    REQUIRE(at1.ok());
    REQUIRE(half.ok());
    CHECK(at0.value().uv == from.uv);
    CHECK(at1.value().uv == to.uv);
    for (const float v : half.value().uv) {
        CHECK(v == 0.4f);
    }
    CHECK(half.value().gatedCells == 42u);

    SECTION("t outside [0, 1] clamps instead of extrapolating") {
        const auto over = render::blendParallaxGrids(from, to, 3.0);
        const auto under = render::blendParallaxGrids(from, to, -2.0);
        REQUIRE(over.ok());
        REQUIRE(under.ok());
        CHECK(over.value().uv == to.uv);
        CHECK(under.value().uv == from.uv);
    }

    SECTION("mismatched layouts, malformed grids and a non-finite t are refused") {
        CHECK_FALSE(render::blendParallaxGrids(from, constantGrid(0.8f, 32, 6), 0.5).ok());
        render::ParallaxWarpGrid shifted = to;
        shifted.latMaxRad = 0.2f;
        CHECK_FALSE(render::blendParallaxGrids(from, shifted, 0.5).ok());
        render::ParallaxWarpGrid broken = to;
        broken.uv.pop_back();
        CHECK_FALSE(render::blendParallaxGrids(from, broken, 0.5).ok());
        CHECK_FALSE(render::blendParallaxGrids(from, to, std::nan("")).ok());
    }
}

TEST_CASE("the applied correction glides across bucket edges instead of stepping", "[parallax][schedule]") {
    // THE property the schedule exists for.  Give each bucket a different
    // measurement and apply what the importer applies to frame f:
    //     blend(G(bucket - 1), G(bucket), parallaxCrossfadeWeight(f)).
    // Between ANY two consecutive frames the applied correction may then move
    // by at most 1/N of the difference between neighbouring measurements - a
    // hard step at a bucket edge (the behaviour this replaces) would move by
    // the whole difference in one frame.
    const std::uint32_t n = render::kParallaxBucketFrames;
    const float measured[] = {0.0f, 1.0f, 0.2f, 0.9f, 0.9f, -0.5f};
    const std::uint32_t buckets = static_cast<std::uint32_t>(std::size(measured));

    float maxNeighbourGap = 0.0f;
    for (std::uint32_t b = 1; b < buckets; ++b) {
        maxNeighbourGap = std::max(maxNeighbourGap, std::fabs(measured[b] - measured[b - 1]));
    }

    float previous = measured[0];
    for (std::uint32_t f = n; f < buckets * n; ++f) {  // from bucket 1, which has a predecessor
        const std::uint32_t b = render::parallaxBucket(f);
        const auto applied = render::blendParallaxGrids(constantGrid(measured[b - 1]), constantGrid(measured[b]),
                                                        render::parallaxCrossfadeWeight(f));
        REQUIRE(applied.ok());
        const float value = applied.value().uv.front();
        INFO("frame " << f << " bucket " << b << " applied " << value << " previous " << previous);
        CHECK(std::fabs(value - previous) <= maxNeighbourGap / static_cast<float>(n) + 1e-6f);
        previous = value;
    }
}

// ---- [WP-TEMPORAL] the glide through refused buckets, and the seam table's --

TEST_CASE("zeroParallaxGridLike keeps the layout and corrects nothing", "[parallax][schedule][temporal]") {
    render::ParallaxWarpGrid like = constantGrid(0.7f);
    like.gatedCells = 9;
    const render::ParallaxWarpGrid zero = render::zeroParallaxGridLike(like);
    REQUIRE(zero.valid());
    CHECK(zero.w == like.w);
    CHECK(zero.h == like.h);
    CHECK(zero.latMinRad == like.latMinRad);
    CHECK(zero.latMaxRad == like.latMaxRad);
    for (const float v : zero.uv) {
        CHECK(v == 0.0f);
    }
    // It glides with its model: a refused bucket fades the grid out, a 1/N
    // step at a time, instead of switching it off in one frame.
    const auto half = render::blendParallaxGrids(like, zero, 0.5);
    REQUIRE(half.ok());
    CHECK(half.value().uv.front() == 0.35f);
    // An invalid model stays invalid (the caller then has nothing to blend).
    CHECK_FALSE(render::zeroParallaxGridLike(render::ParallaxWarpGrid{}).valid());
}

TEST_CASE("blendSeamTables glides where two tables agree and steps where the scene changed",
          "[parallax][schedule][temporal]") {
    using render::blendSeamTables;
    const double noise = render::kSeamTableGlideNoiseDeg;
    const double step = render::kSeamTableStepDeg;
    REQUIRE(noise < step);
    // The cases below call a change of <= 0.2 deg noise and one of >= 2 deg
    // a scene change; the band must keep them on those sides.
    REQUIRE(noise > 0.2);
    REQUIRE(step < 2.0);
    std::vector<float> out;

    SECTION("columns that agree within the noise glide linearly, by t exactly") {
        const std::vector<float> from{1.0f, 2.0f, -1.0f};
        const std::vector<float> to{1.1f, 1.9f, -1.2f};  // all within the noise
        blendSeamTables(&from, &to, 0.25, out);
        REQUIRE(out.size() == 3u);
        for (std::size_t i = 0; i < out.size(); ++i) {
            CHECK(out[i] == Catch::Approx(from[i] + (to[i] - from[i]) * 0.25).margin(1e-6));
        }
    }
    SECTION("a column that changed by more than the step band takes the new table at once") {
        const std::vector<float> from{0.0f, 0.0f};
        const std::vector<float> to{0.1f, 2.0f};  // agrees / a near object arrived
        blendSeamTables(&from, &to, 1.0 / 8.0, out);
        REQUIRE(out.size() == 2u);
        CHECK(out[0] == Catch::Approx(0.1 / 8.0).margin(1e-6));
        CHECK(out[1] == 2.0f);
    }
    SECTION("the weight rises smoothly and monotonically across the band") {
        const std::vector<float> from(64, 0.0f);
        std::vector<float> to(64);
        for (std::size_t i = 0; i < to.size(); ++i) {
            to[i] = static_cast<float>(i) * 0.04f;  // 0 .. 2.52 deg, across the whole band
        }
        blendSeamTables(&from, &to, 0.25, out);
        REQUIRE(out.size() == to.size());
        double previousK = 0.0;
        for (std::size_t i = 1; i < to.size(); ++i) {
            const double k = static_cast<double>(out[i]) / static_cast<double>(to[i]);
            CHECK(k >= 0.25 - 1e-6);
            CHECK(k <= 1.0 + 1e-6);
            CHECK(k >= previousK - 1e-6);
            previousK = k;
        }
        CHECK(previousK == Catch::Approx(1.0));
    }
    SECTION("a missing side is no shift: small columns fade, large ones step") {
        const std::vector<float> table{0.1f, 3.0f};
        blendSeamTables(&table, nullptr, 0.5, out);  // a table fading out into a grid
        REQUIRE(out.size() == 2u);
        CHECK(out[0] == Catch::Approx(0.05).margin(1e-6));
        CHECK(out[1] == 0.0f);
        blendSeamTables(nullptr, &table, 0.5, out);  // fading in
        REQUIRE(out.size() == 2u);
        CHECK(out[0] == Catch::Approx(0.05).margin(1e-6));
        CHECK(out[1] == 3.0f);
        const std::vector<float> empty;
        blendSeamTables(&empty, nullptr, 0.5, out);
        CHECK(out.empty());
        blendSeamTables(nullptr, nullptr, 0.5, out);
        CHECK(out.empty());
    }
    SECTION("two lengths cannot mix: the newer table alone, as if the older were missing") {
        const std::vector<float> from{0.0f, 0.0f, 0.0f};
        const std::vector<float> to{0.1f, 0.1f};
        blendSeamTables(&from, &to, 0.5, out);
        REQUIRE(out.size() == 2u);
        CHECK(out[0] == Catch::Approx(0.05).margin(1e-6));
    }
    SECTION("non-finite inputs never reach the kernel") {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        const std::vector<float> from{nan, 1.0f, inf};
        const std::vector<float> to{0.5f, nan, -inf};
        blendSeamTables(&from, &to, std::nan(""), out);  // a non-finite t: the newer side
        REQUIRE(out.size() == 3u);
        for (const float v : out) {
            CHECK(std::isfinite(v));
        }
        CHECK(out[0] == 0.5f);
        CHECK(out[1] == 0.0f);
    }
    SECTION("an unusable band is a plain linear glide") {
        const std::vector<float> from{0.0f};
        const std::vector<float> to{4.0f};
        blendSeamTables(&from, &to, 0.25, out, 1.0, 0.5);  // inverted
        CHECK(out[0] == 1.0f);
        blendSeamTables(&from, &to, 0.25, out, std::nan(""), step);
        CHECK(out[0] == 1.0f);
        blendSeamTables(&from, &to, 0.25, out, noise, step);  // the real band: a step
        CHECK(out[0] == 4.0f);
    }
    SECTION("the output reuses its capacity") {
        out.reserve(4096);
        const auto* data = out.data();
        const std::vector<float> table(2048, 0.1f);
        blendSeamTables(&table, &table, 0.5, out);
        CHECK(out.data() == data);
        CHECK(out.size() == 2048u);
    }
}

TEST_CASE("blendSeamTables steps only where both tables' measurements are confident",
          "[parallax][schedule][temporal]") {
    using render::blendSeamTables;
    const double noise = render::kSeamTableGlideNoiseDeg;
    const double step = render::kSeamTableStepDeg;
    REQUIRE(render::kSeamTableStepConfLo < render::kSeamTableStepConfHi);
    REQUIRE(render::kSeamTableStepConfLo > 0.0);
    REQUIRE(render::kSeamTableStepConfHi <= 1.0);
    // Every column changes by 3 deg - far past the step band - so without the
    // gate every one would step at the anchor.
    const std::vector<float> from(5, 0.0f);
    const std::vector<float> to(5, 3.0f);
    const double t = 0.25;
    std::vector<float> out;

    SECTION("confident on both sides: exactly the ungated rule") {
        const std::vector<float> sure(5, 1.0f);
        std::vector<float> legacy;
        blendSeamTables(&from, &to, t, legacy);
        blendSeamTables(&from, &to, t, out, noise, step, &sure, &sure);
        REQUIRE(out == legacy);
        CHECK(out[0] == 3.0f);
    }
    SECTION("either side unsure: a plain glide by t") {
        const std::vector<float> sure(5, 1.0f);
        const std::vector<float> unsure(5, 0.1f);
        blendSeamTables(&from, &to, t, out, noise, step, &unsure, &sure);
        CHECK(out[0] == Catch::Approx(3.0 * t).margin(1e-6));
        blendSeamTables(&from, &to, t, out, noise, step, &sure, &unsure);
        CHECK(out[0] == Catch::Approx(3.0 * t).margin(1e-6));
    }
    SECTION("the step grows smoothly and monotonically with the smaller confidence") {
        const std::vector<float> sure(5, 1.0f);
        const std::vector<float> ramp{0.2f, 0.35f, 0.45f, 0.55f, 0.7f};
        blendSeamTables(&from, &to, t, out, noise, step, &ramp, &sure);
        REQUIRE(out.size() == 5u);
        CHECK(out[0] == Catch::Approx(3.0 * t).margin(1e-6));  // below the gate
        CHECK(out[4] == 3.0f);                                  // above it
        for (std::size_t i = 1; i < out.size(); ++i) {
            CHECK(out[i] >= out[i - 1]);
        }
        CHECK(out[2] > 3.0f * static_cast<float>(t));
        CHECK(out[2] < 3.0f);
    }
    SECTION("no confidence, or one of the wrong length, is the ungated rule") {
        std::vector<float> legacy;
        blendSeamTables(&from, &to, t, legacy);
        const std::vector<float> shortConf(3, 0.0f);
        blendSeamTables(&from, &to, t, out, noise, step, &shortConf, nullptr);
        CHECK(out == legacy);
        const std::vector<float> empty;
        blendSeamTables(&from, &to, t, out, noise, step, &empty, &empty);
        CHECK(out == legacy);
    }
    SECTION("a missing table side leaves the gate to the other side's confidence") {
        // Fading in from a bucket whose grid was accepted (no table): an
        // unsure new table glides in, a sure one steps in.
        const std::vector<float> ignored(5, 0.0f);  // belongs to no table: never read
        const std::vector<float> unsure(5, 0.1f);
        const std::vector<float> sure(5, 1.0f);
        blendSeamTables(nullptr, &to, t, out, noise, step, &ignored, &unsure);
        CHECK(out[0] == Catch::Approx(3.0 * t).margin(1e-6));
        blendSeamTables(nullptr, &to, t, out, noise, step, &ignored, &sure);
        CHECK(out[0] == 3.0f);
    }
    SECTION("non-finite confidences glide, out-of-range ones are clamped") {
        const std::vector<float> sure(5, 1.0f);
        const std::vector<float> odd{std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                     -2.0f, 7.0f, 0.0f};
        blendSeamTables(&from, &to, t, out, noise, step, &odd, &sure);
        CHECK(out[0] == Catch::Approx(3.0 * t).margin(1e-6));  // NaN: unsure
        CHECK(out[1] == Catch::Approx(3.0 * t).margin(1e-6));  // inf: not a confidence, unsure
        CHECK(out[2] == Catch::Approx(3.0 * t).margin(1e-6));  // clamped to 0
        CHECK(out[3] == 3.0f);                                  // clamped to 1
        for (const float v : out) {
            CHECK(std::isfinite(v));
        }
    }
    SECTION("small changes glide whatever the confidence") {
        const std::vector<float> near(5, 0.2f);  // within the noise band
        const std::vector<float> sure(5, 1.0f);
        blendSeamTables(&from, &near, t, out, noise, step, &sure, &sure);
        CHECK(out[0] == Catch::Approx(0.2 * t).margin(1e-6));
    }
    SECTION("the agreement test switched off is a plain glide, gate or not") {
        const std::vector<float> sure(5, 1.0f);
        blendSeamTables(&from, &to, t, out, 1.0, 0.5, &sure, &sure);  // inverted band
        CHECK(out[0] == Catch::Approx(3.0 * t).margin(1e-6));
    }
}

TEST_CASE("a refused bucket is glided through, and only a large table change steps at the anchor",
          "[parallax][schedule][temporal]") {
    // What applyAnalyses applies to frame f of bucket b, glided from b - 1:
    //   warp  = blend(grid(b - 1) or a zero grid, grid(b) or a zero grid, w)
    //   table = blendSeamTables(table of a refused b - 1, table of a refused b, w)
    // Buckets: accepted, refused, refused, accepted.  The warp must never move
    // by more than 1/N of the gap between neighbouring corrections per frame,
    // whatever is refused; a table within the noise must glide the same way,
    // and one that changes by a scene's worth must step at the anchor only.
    const std::uint32_t n = render::kParallaxBucketFrames;
    const float gridValue[] = {0.8f, 0.0f, 0.0f, -0.4f};
    const bool accepted[] = {true, false, false, true};
    for (const float tableDelta : {0.2f, 3.0f}) {
        INFO("second refused bucket's table differs from the first's by " << tableDelta << " deg");
        const std::vector<std::vector<float>> tables{{}, {0.1f}, {0.1f + tableDelta}, {}};
        // The table each bucket stands for: none where its grid was accepted.
        const float level[] = {0.0f, 0.1f, 0.1f + tableDelta, 0.0f};
        float previousWarp = gridValue[0];
        float previousTable = 0.0f;
        std::vector<float> table;
        for (std::uint32_t f = n; f < 4 * n; ++f) {
            const std::uint32_t b = render::parallaxBucket(f);
            const double w = render::parallaxCrossfadeWeight(f);
            // ---- the warp, a refused side standing for a zero grid ----
            const render::ParallaxWarpGrid from = constantGrid(accepted[b - 1] ? gridValue[b - 1] : 0.0f);
            const render::ParallaxWarpGrid to = constantGrid(accepted[b] ? gridValue[b] : 0.0f);
            const auto warp = render::blendParallaxGrids(from, to, w);
            REQUIRE(warp.ok());
            const float warpValue = warp.value().uv.front();
            CHECK(std::fabs(warpValue - previousWarp) <= 0.8f / static_cast<float>(n) + 1e-6f);
            previousWarp = warpValue;
            // ---- the table, on the refused sides only ----
            render::blendSeamTables(accepted[b - 1] ? nullptr : &tables[b - 1], accepted[b] ? nullptr : &tables[b],
                                    w, table);
            const float tableValue = table.empty() ? 0.0f : table.front();
            const float gap = std::fabs(level[b] - level[b - 1]);
            INFO("frame " << f << " table " << tableValue << " previous " << previousTable << " gap " << gap);
            if (gap >= render::kSeamTableStepDeg) {
                // The scene changed: the newer side from the anchor on.
                CHECK(tableValue == Catch::Approx(level[b]).margin(1e-6));
            } else {
                // Within the noise: a glide, 1/N of the gap per frame.
                REQUIRE(gap <= render::kSeamTableGlideNoiseDeg);
                CHECK(std::fabs(tableValue - previousTable) <= gap / static_cast<float>(n) + 1e-6f);
            }
            previousTable = tableValue;
        }
    }
}
