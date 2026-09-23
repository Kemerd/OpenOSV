// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_steady.cpp - the per-clip lens rotation (render/LensAlign.h) and the
// steady clip correction (render/ClipSteady.h).
//
// What is pinned, and why each matters:
//
//   * the rotation fit recovers a known rotation - from ideal cells, with a
//     near object and a drifting sky in the way, and end to end through the
//     real flow on synthetic fisheye frames rendered with a rotated rig - and
//     the fold turns the calibration rig into the true one (sign and half
//     split), so the stitch ends up aligned rather than doubly rotated;
//   * a fit that is not a calibration residual is refused, not applied;
//   * the sample frames depend on the clip alone, the medians are medians,
//     and a median seam is still a valid seam;
//   * Auto holds a static near object still and follows a moving one,
//     measured through the real flow;
//   * the clip correction is a function of its sample frames alone;
//   * on the sample clip: the rotation is the research's 0.36 deg, constant
//     over the clip, and aligns the ground with no grid; the clip correction
//     keeps the alignment of each frame's own grid while nothing moves.

#include "SynthFisheye.h"
#include "TestSample.h"

#include "osv/container/OsvFile.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/ClipSteady.h"
#include "osv/render/LensAlign.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/render/SeamCarve.h"
#include "osv/video/DualStreamReader.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <vector>

using namespace osv;
using namespace osv::testsynth;

namespace {

/// The research's rotation on the sample clip (0.36 deg), used as the known
/// synthetic truth.
const Vec3d kTrueW{0.0016, 0.0003, -0.0061};

/// Local east / north unit vectors at (lon, lat) of the polar-axis frame.
void basis(double lon, double lat, Vec3d& eLon, Vec3d& eLat) {
    eLon = Vec3d{std::cos(lon), 0.0, -std::sin(lon)};
    eLat = Vec3d{-std::sin(lat) * std::sin(lon), std::cos(lat), -std::sin(lat) * std::cos(lon)};
}

/// Ideal raw cells of a grid whose flow is the rotation `w` (plus `extra` per
/// cell), 256 x 32 over +-6 deg, every cell trusted and well populated.
render::ParallaxCellStats rotationCells(const Vec3d& w,
                                        const std::function<void(double lonDeg, double& east, double& north,
                                                                 float& gate)>& extra = {}) {
    render::ParallaxCellStats s;
    s.w = 256;
    s.rows = 32;
    s.latTopRad = deg2rad(6.0);
    s.latStepRad = deg2rad(12.0) / 31.0;
    const std::size_t n = static_cast<std::size_t>(s.w) * s.rows;
    s.halfFlow.assign(n * 2u, 0.0f);
    s.pixels.assign(n, 40u);
    s.gate.assign(n, 1.0f);
    for (std::uint32_t r = 0; r < s.rows; ++r) {
        const double lat = s.latTopRad - r * s.latStepRad;
        for (std::uint32_t c = 0; c < s.w; ++c) {
            const double lon = c * kTwoPi / s.w - kPi;
            Vec3d eLon, eLat;
            basis(lon, lat, eLon, eLat);
            // The model: east = w . e_lat, north = -w . e_lon (true angles).
            double east = w.dot(eLat);
            double north = -w.dot(eLon);
            float gate = 1.0f;
            if (extra) {
                extra(rad2deg(lon), east, north, gate);
            }
            const std::size_t i = static_cast<std::size_t>(r) * s.w + c;
            // The grid stores HALF the disparity, longitude in longitude radians.
            s.halfFlow[i * 2u + 0u] = static_cast<float>(0.5 * east / std::cos(lat));
            s.halfFlow[i * 2u + 1u] = static_cast<float>(0.5 * north);
            s.gate[i] = gate;
        }
    }
    return s;
}

/// Angle (degrees) between two rotation vectors.
double angleBetweenDeg(const Vec3d& a, const Vec3d& b) { return rad2deg((a - b).norm()); }

/// A far-field scene with texture at several scales everywhere: what the
/// flow needs to lock onto, with nothing near the camera (pure rotation).
void texturedSky(int /*lens*/, const Vec3d& d, double /*thetaDeg*/, double rgb[3]) {
    const double v = 0.18 * (1.0 + 0.35 * std::sin(23.0 * d.x + 3.0 * d.z) * std::cos(19.0 * d.y + 1.3) +
                             0.25 * std::sin(61.0 * d.z + 7.0 * d.y) + 0.2 * std::cos(97.0 * d.x - 41.0 * d.y) +
                             0.12 * std::sin(173.0 * d.y + 131.0 * d.z));
    rgb[0] = rgb[1] = rgb[2] = std::max(0.01, v);
}

/// Synthetic uncorrected bands: lens 0 shows a texture displaced by
/// `disparity(col)` rows one way, lens 1 the other way (co-visible
/// everywhere).  2048 x 68 rows over +-6 deg, as the parallax band.
render::LensBands syntheticBands(const std::function<double(std::uint32_t col)>& disparity, std::uint32_t seed) {
    render::LensBands b;
    b.w = 2048;
    b.mapH = 1024;
    b.h = 68;
    b.rowOffset = b.mapH / 2u - b.h / 2u;
    const std::size_t n = static_cast<std::size_t>(b.w) * b.h;
    // A band-limited random texture, sampled bilinearly at shifted rows.
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const std::uint32_t tw = b.w, th = b.h + 40u;
    std::vector<float> tex(static_cast<std::size_t>(tw) * th);
    for (float& t : tex) {
        t = uni(rng);
    }
    // Two box passes: correlation length of a few pixels.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> tmp(tex.size());
        for (std::uint32_t y = 0; y < th; ++y) {
            for (std::uint32_t x = 0; x < tw; ++x) {
                float s = 0.0f;
                for (int k = -1; k <= 1; ++k) {
                    s += tex[static_cast<std::size_t>(y) * tw + (x + tw + k) % tw];
                    s += tex[static_cast<std::size_t>(std::clamp<int>(static_cast<int>(y) + k, 0, th - 1)) * tw + x];
                }
                tmp[static_cast<std::size_t>(y) * tw + x] = s / 6.0f;
            }
        }
        tex.swap(tmp);
    }
    const auto sampleTex = [&](std::uint32_t x, double y) {
        const double yy = std::clamp(y + 20.0, 0.0, static_cast<double>(th - 2));
        const std::uint32_t y0 = static_cast<std::uint32_t>(yy);
        const double t = yy - y0;
        return static_cast<float>(tex[static_cast<std::size_t>(y0) * tw + x] * (1.0 - t) +
                                  tex[static_cast<std::size_t>(y0 + 1) * tw + x] * t);
    };
    for (int l = 0; l < 2; ++l) {
        b.luma[l].assign(n, 0.0f);
        b.alpha[l].assign(n, 1.0f);
    }
    for (std::uint32_t y = 0; y < b.h; ++y) {
        for (std::uint32_t x = 0; x < b.w; ++x) {
            const double d = disparity(x);
            const std::size_t i = static_cast<std::size_t>(y) * b.w + x;
            b.luma[0][i] = 0.3f + 0.4f * sampleTex(x, y - 0.5 * d);
            b.luma[1][i] = 0.3f + 0.4f * sampleTex(x, y + 0.5 * d);
        }
    }
    return b;
}

/// The sample clip's calibration rig and a reader.
struct SampleClip {
    std::unique_ptr<OsvFile> file;
    meta::MetadataTrack track;
    meta::FormatInfo format;
    geom::LensRig rig;
    std::vector<std::uint32_t> sync;
};

Result<SampleClip> openSample() {
    SampleClip c;
    OSV_TRY_ASSIGN(OsvFile f, OsvFile::open(osvtest::sampleOsv()));
    c.file = std::make_unique<OsvFile>(std::move(f));
    OSV_TRY_ASSIGN(c.track, meta::MetadataTrack::load(*c.file));
    OSV_TRY_ASSIGN(c.format, meta::FormatDetector::detect(*c.file, &c.track));
    OSV_TRY_ASSIGN(meta::CalibrationSet cal, meta::CalibrationSelector::select(c.track.stream()));
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(static_cast<int>(c.format.streamW), static_cast<int>(c.format.streamH),
                                               static_cast<int>(c.format.sensorW), static_cast<int>(c.format.sensorH),
                                               c.format.digitalFocalLength, 0.5 * (cal.slave.fx + cal.master.fx)));
    OSV_TRY_ASSIGN(c.rig, geom::LensRig::build(cal, scaling, geom::FocalSource::DigitalFocalLength,
                                               c.format.digitalFocalLength, geom::ExtrinsicConvention{}));
    if (const TrackInfo* t = c.file->track(c.format.videoTrackIds[0]); t && t->samples.hasSyncTable()) {
        c.sync = t->samples.syncSamples();
    }
    return c;
}

/// Overlap NCC of columns [c0, c1] (of 2048) and |lat| <= 4 deg, rendered
/// through the kernel with `warp` (null: no grid) - the osvtool seam metric.
double regionNcc(const geom::LensRig& rig, const video::FramePair& pair, const geom::BlendParams& blend,
                 const render::WarpGridView* warp, int c0, int c1, ThreadPool& pool) {
    render::BandParams band;
    band.bandHalfDeg = 4.0;
    auto bands = render::renderLensBands(rig, pair, blend, band, false, nullptr, pool, warp);
    REQUIRE(bands.ok());
    const render::LensBands& b = bands.value();
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, n = 0;
    for (std::uint32_t r = 0; r < b.h; ++r) {
        for (int c = c0; c <= c1; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * b.w + static_cast<std::size_t>(c);
            if (b.alpha[0][i] > 0.5f && b.alpha[1][i] > 0.5f) {
                const double va = b.luma[0][i];
                const double vb = b.luma[1][i];
                sa += va;
                sb += vb;
                saa += va * va;
                sbb += vb * vb;
                sab += va * vb;
                n += 1.0;
            }
        }
    }
    REQUIRE(n > 100.0);
    const double ma = sa / n, mb = sb / n;
    return (sab / n - ma * mb) / std::sqrt((saa / n - ma * ma) * (sbb / n - mb * mb));
}

/// A grid as a view.
render::WarpGridView viewOf(const render::ParallaxWarpGrid& g) {
    return render::WarpGridView{g.uv.data(), g.w, g.h, g.latMinRad, g.latMaxRad};
}

}  // namespace

// ===========================================================================
//  1. The rotation fit
// ===========================================================================

TEST_CASE("the rotation fit recovers a known rotation from a grid's raw cells", "[steady][lensalign]") {
    SECTION("ideal cells") {
        auto fit = render::fitLensRotation(rotationCells(kTrueW));
        REQUIRE(fit.ok());
        CHECK(angleBetweenDeg(fit.value().wRad, kTrueW) < 1e-4);
        CHECK(fit.value().angleDeg == Catch::Approx(rad2deg(kTrueW.norm())).margin(1e-4));
        CHECK(fit.value().residualRmsDeg < 1e-4);
        CHECK(fit.value().inliers == fit.value().cells);
    }
    SECTION("noise, a near object and a drifting sky in the way") {
        std::mt19937 rng(7);
        std::normal_distribution<double> noise(0.0, deg2rad(0.02));
        auto cells = rotationCells(kTrueW, [&](double lonDeg, double& east, double& north, float& gate) {
            east += noise(rng);
            north += noise(rng);
            // The wing: 30 deg of longitude whose flow follows a near object
            // (half a degree along the meridian) - the robust fit's outliers.
            if (lonDeg > 150.0 && lonDeg < 180.0) {
                north += deg2rad(0.5);
            }
            // Open sky: DIS drifts by degrees there, but the benefit gate did
            // not trust it, so the fit must not see it at all.
            if (lonDeg > -150.0 && lonDeg < -60.0) {
                east += deg2rad(3.0);
                gate = 0.0f;
            }
        });
        auto fit = render::fitLensRotation(cells);
        REQUIRE(fit.ok());
        INFO(render::describeLensRotation(fit.value()));
        CHECK(angleBetweenDeg(fit.value().wRad, kTrueW) < 0.005);
        // The near object's cells dropped out: fewer inliers than cells.
        CHECK(fit.value().inliers < fit.value().cells);
        CHECK(fit.value().residualRmsDeg < 0.05);
    }
}

TEST_CASE("the rotation fit refuses what is not a calibration residual", "[steady][lensalign]") {
    SECTION("too few trusted cells (open sky)") {
        auto cells = rotationCells(kTrueW, [](double, double&, double&, float& gate) { gate = 0.0f; });
        auto fit = render::fitLensRotation(cells);
        REQUIRE_FALSE(fit.ok());
        CHECK(fit.error().code == ErrorCode::Unsupported);
    }
    SECTION("the trusted cells cover too short an arc to pin every axis") {
        auto cells = rotationCells(kTrueW, [](double lonDeg, double&, double&, float& gate) {
            gate = (lonDeg > 10.0 && lonDeg < 22.0) ? 1.0f : 0.0f;
        });
        auto fit = render::fitLensRotation(cells);
        REQUIRE_FALSE(fit.ok());
        CHECK(fit.error().code == ErrorCode::Unsupported);
    }
    SECTION("the cells disagree with any single rotation") {
        std::mt19937 rng(11);
        std::normal_distribution<double> wild(0.0, deg2rad(1.0));
        auto cells = rotationCells(kTrueW, [&](double, double& east, double& north, float&) {
            east = wild(rng);
            north = wild(rng);
        });
        auto fit = render::fitLensRotation(cells);
        REQUIRE_FALSE(fit.ok());
    }
    SECTION("an implausibly large rotation") {
        auto fit = render::fitLensRotation(rotationCells(kTrueW * 10.0));  // 3.6 degrees
        REQUIRE_FALSE(fit.ok());
    }
    SECTION("malformed statistics") {
        render::ParallaxCellStats bad = rotationCells(kTrueW);
        bad.gate.pop_back();
        auto fit = render::fitLensRotation(bad);
        REQUIRE_FALSE(fit.ok());
        CHECK(fit.error().code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("the clip's rotation is the median of agreeing frames, and none when they disagree", "[steady][lensalign]") {
    render::LensRotationFit a, b, c;
    a.wRad = kTrueW;
    b.wRad = kTrueW + Vec3d{deg2rad(0.004), 0.0, 0.0};
    c.wRad = kTrueW - Vec3d{0.0, deg2rad(0.003), 0.0};
    auto combined = render::combineLensRotations({a, b, c});
    REQUIRE(combined.ok());
    CHECK(combined.value().fits == 3u);
    CHECK(angleBetweenDeg(combined.value().wRad, kTrueW) < 0.005);
    CHECK(combined.value().spreadDeg < 0.01);

    // One frame a tenth of a degree off: not one rigid rotation.
    render::LensRotationFit off = a;
    off.wRad = kTrueW + Vec3d{deg2rad(0.1), 0.0, 0.0};
    CHECK_FALSE(render::combineLensRotations({a, off, off}).ok());
    // A single frame is not enough by default ...
    CHECK_FALSE(render::combineLensRotations({a}).ok());
    // ... and no finite frame at all is nothing.
    render::LensRotationFit nan = a;
    nan.wRad = Vec3d{std::nan(""), 0.0, 0.0};
    CHECK_FALSE(render::combineLensRotations({nan, nan}).ok());
}

TEST_CASE("the fold turns half the rotation into each lens, in opposite senses", "[steady][lensalign]") {
    auto rig = makeSyntheticRig(512);
    REQUIRE(rig.ok());
    geom::LensRig folded = rig.value();
    REQUIRE(render::applyLensRotation(folded, kTrueW).ok());
    // The master samples body direction d at R(+w/2) d, the slave at R(-w/2) d.
    const Vec3d d = Vec3d{0.3, 0.1, 0.95}.normalized();
    const Mat3d plus = render::rotationFromVector(kTrueW * 0.5);
    const Mat3d minus = render::rotationFromVector(kTrueW * -0.5);
    const Vec3d wantMaster = rig.value().bodyToLens[geom::kMasterLens] * (plus * d);
    const Vec3d wantSlave = rig.value().bodyToLens[geom::kSlaveLens] * (minus * d);
    CHECK((folded.bodyToLens[geom::kMasterLens] * d - wantMaster).norm() < 1e-12);
    CHECK((folded.bodyToLens[geom::kSlaveLens] * d - wantSlave).norm() < 1e-12);
    // Still rotations.
    CHECK(folded.bodyToLens[geom::kMasterLens].determinant() == Catch::Approx(1.0).margin(1e-12));

    // Garbage never reaches the rig.
    geom::LensRig untouched = rig.value();
    CHECK_FALSE(render::applyLensRotation(untouched, Vec3d{std::nan(""), 0.0, 0.0}).ok());
    CHECK_FALSE(render::applyLensRotation(untouched, Vec3d{deg2rad(11.0), 0.0, 0.0}).ok());
    CHECK(untouched.bodyToLens[geom::kMasterLens].distance(rig.value().bodyToLens[geom::kMasterLens]) == 0.0);
}

TEST_CASE("the rotation fit recovers a known rotation through the real flow on synthetic fisheyes",
          "[steady][lensalign][synthetic]") {
    ThreadPool pool;
    auto calibration = makeSyntheticRig(1024);
    REQUIRE(calibration.ok());
    // The lenses as they really are: the calibration with the rotation folded
    // in.  The frames are rendered with THEM, the analysis sees only the
    // calibration - so the fit must recover exactly the fold that turns one
    // into the other.
    geom::LensRig truth = calibration.value();
    REQUIRE(render::applyLensRotation(truth, kTrueW).ok());
    const SynthPair frame = synthPair(truth, texturedSky, pool);

    geom::BlendParams blend;
    blend.useOcclusionMask = false;
    render::ParallaxWarpParams pw;
    pw.backend = render::FlowBackendKind::Classical;
    render::LensRotationParams rp;
    rp.minFits = 1;  // one synthetic frame
    const render::ClipFrameSource source = [&](std::uint32_t) { return Result<video::FramePair>(frame.pair); };
    auto measured = render::measureLensRotation(calibration.value(), blend, {0}, source, pw, rp, pool);
    REQUIRE(measured.ok());
    INFO("refusals: " << (measured.value().refusals.empty() ? "" : measured.value().refusals.front()) << " / "
                      << measured.value().reason);
    REQUIRE(measured.value().accepted);
    const render::LensRotationFit& fit = measured.value().fit;
    INFO(render::describeLensRotation(fit));
    CHECK(angleBetweenDeg(fit.wRad, kTrueW) < 0.02);
    CHECK(fit.angleDeg == Catch::Approx(rad2deg(kTrueW.norm())).margin(0.02));

    // Folded in, nothing is left to fit.
    geom::LensRig aligned = calibration.value();
    REQUIRE(render::applyLensRotation(aligned, fit.wRad).ok());
    auto again = render::measureLensRotation(aligned, blend, {0}, source, pw, rp, pool);
    REQUIRE(again.ok());
    if (again.value().accepted) {
        CHECK(again.value().fit.angleDeg < 0.02);
    }
}

// ===========================================================================
//  2. Sample frames and medians
// ===========================================================================

TEST_CASE("the sample frames depend on the clip alone", "[steady][clip]") {
    // A short clip: the exact targets, decoded straight through.
    CHECK(render::clipSampleFrames(65, {0, 60}, 9) ==
          std::vector<std::uint32_t>{0, 8, 16, 24, 32, 40, 48, 56, 64});
    CHECK(render::clipSampleFrames(65, {0, 60}, 3, 0.1, 0.9) == std::vector<std::uint32_t>{6, 32, 58});
    // A long clip: every target snapped to its nearest sync frame.
    std::vector<std::uint32_t> sync;
    for (std::uint32_t s = 0; s < 3600; s += 60) {
        sync.push_back(s);
    }
    const auto longClip = render::clipSampleFrames(3600, sync, 9);
    REQUIRE(longClip.size() == 9u);
    for (const std::uint32_t f : longClip) {
        CHECK(f % 60u == 0u);
    }
    CHECK(longClip.front() == 0u);
    CHECK(longClip.back() == 3540u);
    // Deterministic, degenerate inputs handled.
    CHECK(render::clipSampleFrames(3600, sync, 9) == longClip);
    CHECK(render::clipSampleFrames(0, sync, 9).empty());
    CHECK(render::clipSampleFrames(5, {}, 9) == std::vector<std::uint32_t>{0, 1, 2, 3, 4});
    CHECK(render::clipSampleFrames(100, {}, 1) == std::vector<std::uint32_t>{50});
}

TEST_CASE("the clip correction's medians are medians, and a median seam is a valid seam", "[steady][clip]") {
    SECTION("grid") {
        render::ParallaxWarpGrid a;
        a.w = 8;
        a.h = 4;
        a.latMinRad = 0.1f;
        a.latMaxRad = -0.1f;
        a.uv.assign(64, 0.0f);
        render::ParallaxWarpGrid b = a, c = a;
        for (std::size_t i = 0; i < a.uv.size(); ++i) {
            a.uv[i] = 1.0f;
            b.uv[i] = 5.0f;
            c.uv[i] = static_cast<float>(i);
        }
        auto m = render::clipParallaxGrid({&a, &b, &c, nullptr});
        REQUIRE(m.ok());
        for (std::size_t i = 0; i < m.value().uv.size(); ++i) {
            const float want = std::clamp(static_cast<float>(i), 1.0f, 5.0f);
            CHECK(m.value().uv[i] == want);
        }
        render::ParallaxWarpGrid other = a;
        other.latMaxRad = -0.2f;
        CHECK_FALSE(render::clipParallaxGrid({&a, &other}).ok());
        CHECK_FALSE(render::clipParallaxGrid({}).ok());
    }
    SECTION("seam table") {
        const std::vector<float> a{0.0f, 1.0f, 2.0f}, b{2.0f, 1.0f, 0.0f}, c{1.0f, 5.0f, 1.0f};
        auto m = render::clipSeamTable({&a, &b, &c});
        REQUIRE(m.ok());
        CHECK(m.value() == std::vector<float>{1.0f, 1.0f, 1.0f});
        const std::vector<float> shorter{1.0f};
        CHECK_FALSE(render::clipSeamTable({&a, &shorter}).ok());
    }
    SECTION("carved seam") {
        // Three seams that each move at most 2 rows (of 0.1 deg) per column;
        // their median must too.
        const std::uint32_t cols = 64;
        std::vector<render::BlendSeam> seams(3);
        std::mt19937 rng(5);
        std::uniform_int_distribution<int> step(-2, 2);
        for (render::BlendSeam& s : seams) {
            s.columns = cols;
            s.table.assign(cols * 2u, 0.0f);
            s.nearWeight.assign(cols, 0.5f);
            s.edgeRad = 0.01f;
            int row = 0;
            for (std::uint32_t c = 0; c < cols; ++c) {
                row = std::clamp(row + step(rng), -20, 20);
                s.table[c * 2u] = static_cast<float>(deg2rad(0.1 * row));
                s.table[c * 2u + 1u] = static_cast<float>(deg2rad(0.5));
            }
        }
        auto m = render::clipBlendSeam({&seams[0], &seams[1], &seams[2]});
        REQUIRE(m.ok());
        CHECK(m.value().valid());
        for (std::uint32_t c = 1; c < cols; ++c) {
            const double d = rad2deg(std::fabs(m.value().table[c * 2u] - m.value().table[(c - 1) * 2u]));
            CHECK(d <= 0.2 + 1e-5);
        }
    }
}

// ===========================================================================
//  3. Auto
// ===========================================================================

TEST_CASE("Auto holds a static near object still and follows one that moves", "[steady][auto]") {
    ThreadPool pool;
    render::ParallaxWarpParams pw;
    pw.backend = render::FlowBackendKind::Classical;
    const auto decide = [&](const std::function<double(std::uint32_t k, std::uint32_t col)>& disparity) {
        // Five samples, each with its own texture (the scene changes) and the
        // near object's disparity as given; far content has none.
        std::vector<render::LensBands> bands;
        std::vector<std::shared_ptr<render::ParallaxWarpGrid>> grids;
        for (std::uint32_t k = 0; k < 5; ++k) {
            bands.push_back(syntheticBands([&](std::uint32_t col) { return disparity(k, col); }, 100u + k));
            auto g = render::parallaxFromBands(bands.back(), pw, &pool);
            REQUIRE(g.ok());
            grids.push_back(std::make_shared<render::ParallaxWarpGrid>(std::move(g).value()));
        }
        std::vector<const render::ParallaxWarpGrid*> ptrs;
        for (const auto& g : grids) {
            ptrs.push_back(g.get());
        }
        auto clip = render::clipParallaxGrid(ptrs);
        REQUIRE(clip.ok());
        const render::WarpGridView clipView = viewOf(clip.value());
        render::SeamCorrection clipCorr;
        clipCorr.warp = &clipView;
        std::vector<render::WarpGridView> views;
        views.reserve(grids.size());
        std::vector<render::SteadySample> samples;
        for (std::uint32_t k = 0; k < 5; ++k) {
            views.push_back(viewOf(*grids[k]));
            render::SteadySample s;
            s.frame = k * 8u;
            s.bands = &bands[k];
            s.own.warp = &views.back();
            samples.push_back(s);
        }
        auto d = render::decideSteady(samples, clipCorr, render::SteadyDecisionParams{}, &pool);
        REQUIRE(d.ok());
        return d.value();
    };
    // A near object 4 rows (0.7 deg) of disparity wide, over 100 columns.
    const auto object = [](std::uint32_t col, std::uint32_t start) {
        return (col >= start && col < start + 100u) ? 4.0 : 0.0;
    };

    SECTION("static: the same place in every sample (a wing, a helmet visor)") {
        const render::SteadyDecision d = decide([&](std::uint32_t, std::uint32_t col) { return object(col, 600); });
        INFO(render::describeSteadyDecision(d));
        CHECK(d.steady);
        CHECK(d.judged > 0u);  // the object was judged, not skipped
        CHECK(d.failed == 0u);
    }
    SECTION("moving: somewhere else in every sample (a person walking past)") {
        const render::SteadyDecision d =
            decide([&](std::uint32_t k, std::uint32_t col) { return object(col, 300u + 350u * k); });
        INFO(render::describeSteadyDecision(d));
        CHECK_FALSE(d.steady);
        CHECK(d.failed > 0u);
        CHECK(d.worstKeep < 0.4);
    }
    SECTION("nothing near at all") {
        const render::SteadyDecision d = decide([](std::uint32_t, std::uint32_t) { return 0.0; });
        INFO(render::describeSteadyDecision(d));
        CHECK(d.steady);
    }
}

TEST_CASE("decideSteady refuses malformed input", "[steady][auto]") {
    render::SteadySample s;
    s.bands = nullptr;
    CHECK_FALSE(render::decideSteady({s}, render::SeamCorrection{}, render::SteadyDecisionParams{}).ok());
    render::SteadyDecisionParams bad;
    bad.sectors = 0;
    CHECK_FALSE(render::decideSteady({}, render::SeamCorrection{}, bad).ok());
    bad = render::SteadyDecisionParams{};
    bad.maxLoss = std::nan("");
    CHECK_FALSE(render::decideSteady({}, render::SeamCorrection{}, bad).ok());
    // Nothing to judge is steady.
    auto none = render::decideSteady({}, render::SeamCorrection{}, render::SteadyDecisionParams{});
    REQUIRE(none.ok());
    CHECK(none.value().steady);
}

// ===========================================================================
//  4. The clip correction on the sample clip
// ===========================================================================

TEST_CASE("on the sample clip the lens rotation is 0.36 deg, the same all through, and aligns the ground",
          "[steady][lensalign][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto clip = openSample();
    REQUIRE(clip.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), clip.value().format);
    REQUIRE(reader.ok());
    ThreadPool pool;
    const geom::BlendParams blend;
    const std::vector<std::uint32_t> frames =
        render::clipSampleFrames(reader.value().frameCount(), clip.value().sync, render::kLensRotationSamples, 0.1,
                                 0.9);
    REQUIRE(frames == std::vector<std::uint32_t>{6, 32, 58});
    render::ParallaxWarpParams pw;
    pw.backend = render::FlowBackendKind::Classical;
    const render::ClipFrameSource source = [&](std::uint32_t f) { return reader.value().read(f); };
    auto measured = render::measureLensRotation(clip.value().rig, blend, frames, source, pw, {}, pool);
    REQUIRE(measured.ok());
    REQUIRE(measured.value().accepted);
    const render::LensRotationFit& fit = measured.value().fit;
    INFO(render::describeLensRotation(fit));
    // The research measured 0.353-0.377 deg from three flows (AI_STITCHING.md 3.3).
    CHECK(fit.angleDeg > 0.33);
    CHECK(fit.angleDeg < 0.40);
    CHECK(fit.fits == 3u);
    CHECK(fit.spreadDeg < 0.02);

    // Frame 32's ground, no grid at all: 0.37 through the calibration alone.
    auto pair = reader.value().read(32);
    REQUIRE(pair.ok());
    geom::LensRig aligned = clip.value().rig;
    REQUIRE(render::applyLensRotation(aligned, fit.wRad).ok());
    const double before = regionNcc(clip.value().rig, pair.value(), blend, nullptr, 1110, 1700, pool);
    const double after = regionNcc(aligned, pair.value(), blend, nullptr, 1110, 1700, pool);
    INFO("ground NCC, no grid: calibration " << before << ", rotation folded " << after);
    CHECK(before < 0.5);
    CHECK(after > 0.86);
}

TEST_CASE("on the sample clip the steady correction keeps each frame's alignment while nothing moves",
          "[steady][clip][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto clip = openSample();
    REQUIRE(clip.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), clip.value().format);
    REQUIRE(reader.ok());
    ThreadPool pool;
    const geom::BlendParams blend;
    render::ClipSteadyParams params;
    params.parallax.backend = render::FlowBackendKind::Classical;
    const std::vector<std::uint32_t> frames =
        render::clipSampleFrames(reader.value().frameCount(), clip.value().sync, render::kClipSteadySamples);
    REQUIRE(frames.size() == 9u);

    // Measured twice from sources that have been read in different orders:
    // the correction is a function of the sample frames alone.
    std::map<std::uint32_t, video::FramePair> decoded;
    for (const std::uint32_t f : frames) {
        auto p = reader.value().read(f);
        REQUIRE(p.ok());
        decoded.emplace(f, std::move(p).value());
    }
    const render::ClipFrameSource fromMap = [&](std::uint32_t f) -> Result<video::FramePair> {
        return decoded.at(f);
    };
    auto a = render::measureClipSteady(clip.value().rig, blend, frames, fromMap, params, pool);
    (void)reader.value().read(64);  // move the reader somewhere else first
    (void)reader.value().read(3);
    const render::ClipFrameSource fromReader = [&](std::uint32_t f) { return reader.value().read(f); };
    auto b = render::measureClipSteady(clip.value().rig, blend, frames, fromReader, params, pool);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(a.value().grid);
    REQUIRE(b.value().grid);
    CHECK(a.value().grid->uv == b.value().grid->uv);
    REQUIRE(a.value().seam);
    REQUIRE(b.value().seam);
    CHECK(a.value().seam->table == b.value().seam->table);
    CHECK(a.value().acceptedGrids == 9u);
    INFO(render::describeSteadyDecision(a.value().decision));
    CHECK(a.value().decision.steady);  // a rigid mount: Auto holds it still

    // Each sample's own grid - what the per-bucket schedule would render its
    // bucket with.
    std::map<std::uint32_t, render::ParallaxWarpGrid> own;
    for (const std::uint32_t f : frames) {
        auto g = render::buildParallaxWarp(clip.value().rig, decoded.at(f), blend, params.parallax, nullptr, pool);
        REQUIRE(g.ok());
        own.emplace(f, std::move(g).value());
    }

    // Frames 0 / 32 / 64: the clip grid aligns the ground and the wing as
    // well as each frame's own grid (research harness: ground 0.9036 vs
    // 0.9045, wing 0.313 vs 0.314), rendered through the kernel here.
    const render::WarpGridView clipView = viewOf(*a.value().grid);
    for (const std::uint32_t f : {0u, 32u, 64u}) {
        const render::WarpGridView ownView = viewOf(own.at(f));
        const video::FramePair& pair = decoded.at(f);
        const double groundOwn = regionNcc(clip.value().rig, pair, blend, &ownView, 1110, 1700, pool);
        const double groundClip = regionNcc(clip.value().rig, pair, blend, &clipView, 1110, 1700, pool);
        const double wingOwn = regionNcc(clip.value().rig, pair, blend, &ownView, 1880, 2040, pool);
        const double wingClip = regionNcc(clip.value().rig, pair, blend, &clipView, 1880, 2040, pool);
        INFO("frame " << f << ": ground own " << groundOwn << " clip " << groundClip << ", wing own " << wingOwn
                      << " clip " << wingClip);
        CHECK(groundClip > groundOwn - 0.005);
        CHECK(wingClip > wingOwn - 0.03);
    }

    // What the per-bucket schedule moves: consecutive sample grids differ by
    // many pixels at the nacelle (the glide spreads it over 8 frames); the
    // clip grid is one grid for every frame, so nothing is left to move.
    double worstPx = 0.0;
    for (std::size_t k = 1; k < frames.size(); ++k) {
        const render::ParallaxWarpGrid& g0 = own.at(frames[k - 1]);
        const render::ParallaxWarpGrid& g1 = own.at(frames[k]);
        REQUIRE(g0.uv.size() == g1.uv.size());
        for (std::uint32_t r = params.parallax.decayRows; r + params.parallax.decayRows < g0.h; ++r) {
            for (std::uint32_t c = 0; c < g0.w; ++c) {
                const double lon = c * 360.0 / g0.w - 180.0;
                if (lon < 150.5 || lon > 178.7) {
                    continue;
                }
                const std::size_t i = (static_cast<std::size_t>(r) * g0.w + c) * 2u;
                const double du = static_cast<double>(g1.uv[i]) - g0.uv[i];
                const double dv = static_cast<double>(g1.uv[i + 1]) - g0.uv[i + 1];
                worstPx = std::max(worstPx, rad2deg(std::hypot(du, dv)) * 6000.0 / 360.0);
            }
        }
    }
    INFO("largest bucket-to-bucket change of the wing grid: " << worstPx << " px at 6K");
    CHECK(worstPx > 4.0);
}
