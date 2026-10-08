// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Seam regression tests on the sample clip: the verified conventions must
// give a well aligned overlap band, and the alternatives must be measurably
// worse.  This is the test that catches a wrong sign anywhere in the
// geometry chain.
//
// The seam-shift table's estimator (searchSeamFromBands) is also tested on
// SYNTHETIC bands, no clip needed: a zero-overlap arc must stay unshifted,
// featureless sky with random correlation peaks must leave the table flat,
// and a confident change between two anchors must still step.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "TestSample.h"

#include "osv/container/OsvFile.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/DualStreamReader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

using namespace osv;

namespace {

struct Loaded {
    std::unique_ptr<OsvFile> file;
    meta::MetadataTrack track;
    meta::FormatInfo format;
    meta::CalibrationSet cal;
    video::FramePair pair;
};

Result<Loaded> load() {
    Loaded l;
    OSV_TRY_ASSIGN(OsvFile f, OsvFile::open(osvtest::sampleOsv()));
    l.file = std::make_unique<OsvFile>(std::move(f));
    OSV_TRY_ASSIGN(l.track, meta::MetadataTrack::load(*l.file));
    OSV_TRY_ASSIGN(l.format, meta::FormatDetector::detect(*l.file, &l.track));
    OSV_TRY_ASSIGN(l.cal, meta::CalibrationSelector::select(l.track.stream()));
    OSV_TRY_ASSIGN(video::DualStreamReader reader, video::DualStreamReader::open(osvtest::sampleOsv(), l.format));
    OSV_TRY_ASSIGN(l.pair, reader.read(0));
    return l;
}

Result<geom::LensRig> buildRig(const Loaded& l, std::optional<double> scaleOverride,
                               geom::RotationSense sense = geom::RotationSense::BodyToLens) {
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(static_cast<int>(l.format.streamW), static_cast<int>(l.format.streamH),
                                               static_cast<int>(l.format.sensorW), static_cast<int>(l.format.sensorH),
                                               l.format.digitalFocalLength, 0.5 * (l.cal.slave.fx + l.cal.master.fx),
                                               scaleOverride));
    geom::ExtrinsicConvention conv;
    conv.sense = sense;
    return geom::LensRig::build(l.cal, scaling, geom::FocalSource::DigitalFocalLength, l.format.digitalFocalLength, conv);
}

// ---------------------------------------------------------------------------
//  Synthetic bands for the seam-table estimator
// ---------------------------------------------------------------------------
//
// The search band of the default SeamSearchParams on a 2048-column map:
// +/- (6 deg + 24 rows) around the seam, i.e. 116 rows of a 1024-row map,
// of which rows 24..91 are matched and the rest is the shift padding.

constexpr std::uint32_t kBandW = 2048;
constexpr std::uint32_t kBandH = 116;
constexpr std::uint32_t kMapH = 1024;
constexpr double kDegPerRow = 180.0 / static_cast<double>(kMapH);

/// Deterministic noise in [0, 1) from integer keys (a splitmix64 chain), so
/// the synthetic scenes are identical on every compiler and standard library
/// (std:: distributions are not).
double hash01(std::uint64_t a, std::uint64_t b, std::uint64_t c = 0, std::uint64_t d = 0) {
    std::uint64_t x = 0x9E3779B97F4A7C15ull;
    for (const std::uint64_t k : {a, b, c, d}) {
        x ^= k + 0x632BE59BD9B4E5ull + (x << 6) + (x >> 2);
        // splitmix64 finaliser: every input bit reaches every output bit.
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        x ^= x >> 31;
    }
    return static_cast<double>(x >> 11) * (1.0 / 9007199254740992.0);
}

/// A textured scene along the meridian: twelve sinusoids with random
/// amplitude (0.004-0.016 code values), frequency (0.25-1.25 rad per row)
/// and phase, each drifting slowly with longitude.  Structure at every
/// scale and no period inside the +/-24-row search, so a match has one clear
/// peak - what a car body or a building gives the real search.
class Scene {
public:
    explicit Scene(std::uint64_t seed) {
        for (std::uint64_t k = 0; k < 12; ++k) {
            m_waves.push_back({0.004 + 0.012 * hash01(seed, k, 1), 0.25 + 1.0 * hash01(seed, k, 2),
                               6.2832 * hash01(seed, k, 3), 0.004 * (hash01(seed, k, 4) - 0.5)});
        }
    }
    /// Luma (D-Log M code value) at a fractional band row and a column.
    [[nodiscard]] double at(double row, double col) const {
        double v = 0.45;
        for (const Wave& w : m_waves) {
            v += w.amp * std::sin(w.omega * row + w.phase + w.drift * col);
        }
        return v;
    }

private:
    struct Wave {
        double amp, omega, phase, drift;
    };
    std::vector<Wave> m_waves;
};

/// An empty band of the search geometry (both lenses fully covering).
render::LensBands emptyBands() {
    render::LensBands b;
    b.w = kBandW;
    b.h = kBandH;
    b.mapH = kMapH;
    b.rowOffset = kMapH / 2 - kBandH / 2;
    for (int lens = 0; lens < 2; ++lens) {
        b.luma[lens].assign(static_cast<std::size_t>(kBandW) * kBandH, 0.0f);
        b.alpha[lens].assign(b.luma[lens].size(), 1.0f);
    }
    return b;
}

/// Both lenses see `scene` everywhere; lens 1 sees it displaced by
/// `dispRows[c]` band rows in column c (positive: lower in the band - a
/// near object, as searchSeam's sign convention has it).
render::LensBands sceneBands(const Scene& scene, const std::vector<double>& dispRows) {
    render::LensBands b = emptyBands();
    for (std::uint32_t r = 0; r < kBandH; ++r) {
        for (std::uint32_t c = 0; c < kBandW; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * kBandW + c;
            b.luma[0][i] = static_cast<float>(scene.at(r, c));
            b.luma[1][i] = static_cast<float>(scene.at(static_cast<double>(r) - dispRows[c], c));
        }
    }
    return b;
}

/// Columns [c0, c1) become featureless sky: a gentle brightness ramp along
/// the rows (3e-4 codes per row, as a clear sky towards the horizon) plus a
/// faint, smooth, lens-specific ripple (sensor noise and compression).  The
/// ramp correlates at almost any shift, so the best NCC is high (~0.95) and
/// its peak lands wherever the ripple puts it.
void makeSky(render::LensBands& b, std::uint32_t c0, std::uint32_t c1) {
    for (std::uint32_t c = c0; c < c1; ++c) {
        for (int lens = 0; lens < 2; ++lens) {
            for (std::uint32_t r = 0; r < kBandH; ++r) {
                double ripple = 0.0;
                const auto key = static_cast<std::uint64_t>(lens);
                for (std::uint64_t k = 0; k < 3; ++k) {
                    const double omega = 0.15 + 0.3 * hash01(99, key, c, k);
                    ripple += 0.0015 * std::sin(omega * r + 6.28 * hash01(99, key, c, k + 9));
                }
                b.luma[lens][static_cast<std::size_t>(r) * kBandW + c] =
                    static_cast<float>(0.6 + 3e-4 * r + ripple / std::sqrt(1.5));
            }
        }
    }
}

/// Columns of a car-mounted clip's zero-overlap arc (the occlusion polygons
/// leave no co-visible pixel on 1764-2047 and 0-270, 555 columns).
bool inArc(std::uint32_t c) { return c >= 1764 || c <= 270; }

/// The arc: lens 0 covers only the rows above the seam gap, lens 1 only those
/// below, and lens 1 shows different content (the other side of the car) -
/// so only large shifts pair anything, and what they pair is unrelated.
void makeArc(render::LensBands& b, const Scene& other) {
    for (std::uint32_t c = 0; c < kBandW; ++c) {
        if (!inArc(c)) {
            continue;
        }
        for (std::uint32_t r = 0; r < kBandH; ++r) {
            const std::size_t i = static_cast<std::size_t>(r) * kBandW + c;
            b.luma[1][i] = static_cast<float>(other.at(r, c));
            b.alpha[0][i] = r <= 52 ? 1.0f : 0.0f;
            b.alpha[1][i] = r >= 64 ? 1.0f : 0.0f;
        }
    }
}

/// Run the estimator with a 4-thread pool (the parallel path the importer
/// takes) and require success.
render::SeamProfile estimate(const render::LensBands& b, const render::SeamSearchParams& sp = {},
                             const std::vector<float>* prior = nullptr) {
    ThreadPool pool(4);
    auto p = render::searchSeamFromBands(b, sp, &pool, prior);
    REQUIRE(p.ok());
    REQUIRE(p.value().columns == kBandW);
    REQUIRE(p.value().shiftDeg.size() == kBandW);
    REQUIRE(p.value().confidence.size() == kBandW);
    return std::move(p).value();
}

}  // namespace

TEST_CASE("verified conventions align the overlap band, alternatives do not", "[render][seam][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto l = load();
    REQUIRE(l.ok());
    ThreadPool pool;
    geom::BlendParams blend;
    render::BandParams band;
    band.bandHalfDeg = 4.0;

    auto rig = buildRig(l.value(), std::nullopt);
    REQUIRE(rig.ok());
    auto ncc = render::overlapNcc(rig.value(), l.value().pair, blend, band, pool);
    REQUIRE(ncc.ok());
    INFO("NCC with verified conventions: " << ncc.value());
    REQUIRE(ncc.value() >= 0.80);

    // Pure 0.78125 scale (no crop): a DIFFERENT geometry, which must still
    // produce a plausible overlap - the 4 deg feathered band is only mildly
    // sensitive to the crop scale, so this is not a discriminator.
    //
    // It was asserted as one until a second sample clip showed the ordering is
    // not stable: on CAM_20260904090647_0010_D.OSV the verified scale won
    // (0.870 vs 0.856), on example_footage_dlogm.OSV it loses (0.825 vs
    // 0.877).  Both clips are correctly stitched by the verified scale, which
    // is confirmed independently below by the LensToBody case and by the
    // absolute threshold above; the band NCC simply does not resolve a 1.6 %
    // scale difference reliably enough to rank two nearly-identical
    // geometries.  Asserting an ordering the measurement cannot support would
    // be pinning one clip's noise, so what is checked is that the alternative
    // scale stays in the same plausible range rather than that it loses.
    //
    // Those numbers were measured while LensRig still took
    // digital_focal_length whenever it sat within 1.2x of a lens's
    // calibration * scale, so the 0.78125 rig carried the 3776 px crop's
    // focal with only its principal point rescaled.  digital_focal_length now
    // has to agree within 0.5 % per lens (8K-mode clips miss by 1.3-2.7 %),
    // and at 0.78125 it misses by 1.7 %, so this rig takes the calibration *
    // 0.78125 focal as well: a real pure-scale geometry with a 1.7 % short
    // focal, measured at 0.779 on the sample (the verified rig: 0.825).  The
    // floor is set for a plausible but mis-scaled overlap; a broken rig (the
    // transposed extrinsics below) sits far under it.
    auto rigScale = buildRig(l.value(), 3000.0 / 3840.0);
    REQUIRE(rigScale.ok());
    auto nccScale = render::overlapNcc(rigScale.value(), l.value().pair, blend, band, pool);
    REQUIRE(nccScale.ok());
    INFO("NCC with 0.78125 scale: " << nccScale.value());
    REQUIRE(nccScale.value() >= 0.72);
    REQUIRE(std::abs(ncc.value() - nccScale.value()) < 0.10);

    // Transposed extrinsics must be much worse.
    auto rigT = buildRig(l.value(), std::nullopt, geom::RotationSense::LensToBody);
    REQUIRE(rigT.ok());
    auto nccT = render::overlapNcc(rigT.value(), l.value().pair, blend, band, pool);
    REQUIRE(nccT.ok());
    INFO("NCC with LensToBody: " << nccT.value());
    REQUIRE(nccT.value() <= 0.6);
}

TEST_CASE("seam search finds small disparities and does not hurt alignment", "[render][seam][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto l = load();
    REQUIRE(l.ok());
    ThreadPool pool;
    geom::BlendParams blend;
    auto rig = buildRig(l.value(), std::nullopt);
    REQUIRE(rig.ok());

    render::SeamSearchParams sp;
    auto profile = render::searchSeam(rig.value(), l.value().pair, blend, sp, pool);
    REQUIRE(profile.ok());
    const render::SeamProfile& p = profile.value();
    REQUIRE(p.columns == 2048);
    INFO("seam meanNcc " << p.meanNcc << ", accepted " << p.acceptedColumns);
    // Mean over accepted columns (measured 0.74 on the sample; featureless sky
    // columns pull it down).
    REQUIRE(p.meanNcc >= 0.65);
    REQUIRE(p.acceptedColumns > p.columns / 2);
    std::vector<float> absShift(p.shiftDeg.size());
    for (std::size_t i = 0; i < absShift.size(); ++i) {
        absShift[i] = std::fabs(p.shiftDeg[i]);
    }
    std::nth_element(absShift.begin(), absShift.begin() + static_cast<std::ptrdiff_t>(absShift.size() / 2), absShift.end());
    const float medianDeg = absShift[absShift.size() / 2];
    INFO("median |shift| = " << medianDeg << " deg");
    // Parallax at DJI's 0.75 m minimum stitching distance is 1.9 deg; the
    // sample clip has near objects (measured median 1.1 deg).
    REQUIRE(medianDeg < 3.0f);

    // Applying the profile must not reduce the overlap correlation.
    render::BandParams band;
    band.bandHalfDeg = 4.0;
    auto before = render::overlapNcc(rig.value(), l.value().pair, blend, band, pool);
    auto after = render::overlapNcc(rig.value(), l.value().pair, blend, band, pool, &p.shiftDeg);
    REQUIRE(before.ok());
    REQUIRE(after.ok());
    INFO("NCC before " << before.value() << " after " << after.value());
    REQUIRE(after.value() >= before.value() - 0.02);
}

TEST_CASE("gain estimate is sane on the sample clip", "[render][seam][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto l = load();
    REQUIRE(l.ok());
    ThreadPool pool;
    geom::BlendParams blend;
    auto rig = buildRig(l.value(), std::nullopt);
    REQUIRE(rig.ok());
    render::BandParams band;
    auto g = render::estimateGain(rig.value(), l.value().pair, blend, band, pool);
    REQUIRE(g.ok());
    REQUIRE(g.value().samples > 1000);
    for (int lens = 0; lens < 2; ++lens) {
        const Vec3d& v = g.value().gain[lens];
        for (const double c : {v.x, v.y, v.z}) {
            REQUIRE(c >= 0.8);
            REQUIRE(c <= 1.25);
        }
    }
    for (const double prod : {g.value().gain[0].x * g.value().gain[1].x, g.value().gain[0].y * g.value().gain[1].y,
                              g.value().gain[0].z * g.value().gain[1].z}) {
        REQUIRE(std::fabs(prod - 1.0) < 0.02);
    }
}

// ===========================================================================
//  The seam-shift table's estimator on synthetic bands (no clip needed)
// ===========================================================================

TEST_CASE("the seam table never shifts a column the two lenses do not both see", "[render][seam][seamtable]") {
    // The ring of a car-mounted clip: textured content at zero disparity, a
    // near object (the hood) at +2.8 deg over 1536-1763, and the 555-column
    // arc where the occlusion polygons leave no overlap.  The old search
    // scored the arc's chance correlations across the polygon gap and the
    // nearest-neighbour fill and Gaussian spread them over the whole arc,
    // which moved car-body content near the cut by 0.3-0.95 deg on the clip.
    const Scene scene(7);
    std::vector<double> disp(kBandW, 0.0);
    for (std::uint32_t c = 1536; c < 1764; ++c) {
        disp[c] = 2.8 / kDegPerRow;
    }
    render::LensBands b = sceneBands(scene, disp);
    makeArc(b, Scene(1007));

    // The arc's columns past the inheritance margin: the first 4 columns
    // of each end still see the overlap through their 9-column window, the
    // next 32 (unmeasuredInheritDeg at 2048 columns) may inherit.
    const auto beyondMargin = [](std::uint32_t c) { return c >= 1764 + 4 + 32 || c <= 270 - 4 - 32; };

    SECTION("pinned (the default): exactly 0 deep in the arc, the hood still fully corrected") {
        const render::SeamProfile p = estimate(b);
        INFO("unmeasured " << p.unmeasuredColumns << ", confident " << p.confidentColumns << ", T(1650) "
                           << p.shiftDeg[1650] << ", T(1750) " << p.shiftDeg[1750] << ", T(1780) "
                           << p.shiftDeg[1780]);
        // The arc minus the few columns whose 9-column window still reaches
        // into the overlap.
        CHECK(p.unmeasuredColumns >= 545);
        CHECK(p.unmeasuredColumns <= 560);
        for (std::uint32_t c = 0; c < kBandW; ++c) {
            if (inArc(c) && beyondMargin(c)) {
                // The calibrated geometry, bit for bit.
                CHECK(p.shiftDeg[c] == 0.0f);
                CHECK(p.confidence[c] == 0.0f);
            }
        }
        // The hood is measured where both lenses see it, right up to the
        // arc: the fade to the pin happens inside the inheritance margin,
        // not on the measured side.
        CHECK(p.shiftDeg[1650] == Catch::Approx(2.8).margin(0.1));
        CHECK(p.shiftDeg[1740] == Catch::Approx(2.8).margin(0.15));
        CHECK(p.confidence[1650] >= 0.9f);
        // ... and it fades monotonically through the margin.
        for (std::uint32_t c = 1765; c < 1800; ++c) {
            CHECK(p.shiftDeg[c] <= p.shiftDeg[c - 1] + 1e-4f);
        }
        // Far from both, the table is the measured 0.
        for (std::uint32_t c = 300; c < 1450; ++c) {
            CHECK(std::fabs(p.shiftDeg[c]) < 0.02f);
        }
    }

    SECTION("pinning at the arc's edge bends the measured hood instead") {
        // No inheritance margin: the pin sits at the arc's edge and the
        // smoother has to bend the confident hood down to it.
        render::SeamSearchParams sp;
        sp.unmeasuredInheritDeg = 0.0;
        const render::SeamProfile p = estimate(b, sp);
        INFO("T(1740) with the pin at the edge " << p.shiftDeg[1740]);
        CHECK(p.shiftDeg[1740] < 2.3f);
        CHECK(p.shiftDeg[1780] == 0.0f);
    }

    SECTION("a soft pull past the margin lets the hood leak on into the arc - why the default pins") {
        render::SeamSearchParams sp;
        sp.unmeasuredPriorWeight = 1.0;
        const render::SeamProfile p = estimate(b, sp);
        float leak = 0.0f;
        for (std::uint32_t c = 0; c < kBandW; ++c) {
            if (inArc(c) && beyondMargin(c)) {
                leak = std::max(leak, std::fabs(p.shiftDeg[c]));
            }
        }
        INFO("largest |shift| past the margin with a soft pull " << leak << " deg");
        CHECK(leak > 0.2f);
        CHECK(p.shiftDeg[1650] == Catch::Approx(2.8).margin(0.1));
    }

    SECTION("without the co-visibility rules the arc's chance matches are scored") {
        // The scene really reproduces the defect: switch both rules off and
        // shifts that pair lens 0 above the gap with lens 1 below it score.
        render::SeamSearchParams sp;
        sp.minCovalidFraction = 0.0;
        sp.unmeasuredFraction = 0.0;
        const render::SeamProfile p = estimate(b, sp);
        std::uint32_t scoredInArc = 0;
        for (std::uint32_t c = 0; c < kBandW; ++c) {
            if (inArc(c) && std::isfinite(p.measuredDeg[c])) {
                ++scoredInArc;
            }
        }
        INFO("arc columns with a measurement when every shift is scored: " << scoredInArc);
        CHECK(scoredInArc > 500);
    }
}

TEST_CASE("featureless sky with random correlation peaks leaves the seam table flat", "[render][seam][seamtable]") {
    // Textured content at zero disparity, and 600 columns of clear sky.  On
    // the car clip's OSV a seam-parallel lamp pole bent by up to 15.5 deg
    // because sky columns like these were accepted at NCC >= 0.5 with random
    // shifts of up to +/-4 deg, and the table carried them out to 12 deg
    // from the seam.
    const Scene scene(11);
    render::LensBands b = sceneBands(scene, std::vector<double>(kBandW, 0.0));
    makeSky(b, 600, 1200);
    const render::SeamProfile p = estimate(b);

    // The scene is the defect: the sky correlates well at a shift that
    // wanders from column to column.
    std::uint32_t passing = 0;
    double sum = 0.0, sumSq = 0.0;
    std::uint32_t measured = 0;
    float skyConf = 0.0f;
    for (std::uint32_t c = 620; c < 1180; ++c) {
        passing += p.ncc[c] >= 0.5f ? 1u : 0u;
        if (std::isfinite(p.measuredDeg[c])) {
            sum += p.measuredDeg[c];
            sumSq += static_cast<double>(p.measuredDeg[c]) * p.measuredDeg[c];
            ++measured;
        }
        skyConf = std::max(skyConf, p.confidence[c]);
    }
    REQUIRE(measured > 500);
    const double mean = sum / measured;
    const double spread = std::sqrt(std::max(0.0, sumSq / measured - mean * mean));
    INFO("sky: " << passing << " of 560 columns at NCC >= 0.5, raw shifts spread " << spread
                 << " deg, largest confidence " << skyConf);
    CHECK(passing > 500);
    CHECK(spread > 1.0);
    // ... and none of it is trusted, so the table stays flat.
    CHECK(skyConf < 0.05f);
    float tableMax = 0.0f;
    for (std::uint32_t c = 0; c < kBandW; ++c) {
        tableMax = std::max(tableMax, std::fabs(p.shiftDeg[c]));
    }
    INFO("largest |shift| anywhere " << tableMax << " deg");
    CHECK(tableMax < 0.1f);
    // The textured columns are trusted.
    CHECK(p.confidence[300] >= 0.9f);
    CHECK(p.confidence[1600] >= 0.9f);
}

TEST_CASE("a match resting on a thin overlap is measured unshifted but earns little weight",
          "[render][seam][seamtable]") {
    // The maintainer's sample along its selfie-stick arc: the occlusion
    // polygons leave 18 co-visible rows of the 68 matched (lens 0 from row
    // 51 down, lens 1 down to row 68).  The content agrees unshifted.
    const Scene scene(43);
    render::LensBands b = sceneBands(scene, std::vector<double>(kBandW, 0.0));
    for (std::uint32_t c = 900; c < 1100; ++c) {
        for (std::uint32_t r = 0; r < kBandH; ++r) {
            const std::size_t i = static_cast<std::size_t>(r) * kBandW + c;
            b.alpha[0][i] = r >= 51 ? 1.0f : 0.0f;
            b.alpha[1][i] = r <= 68 ? 1.0f : 0.0f;
        }
    }
    const render::SeamProfile p = estimate(b);
    for (std::uint32_t c = 910; c < 1090; ++c) {
        INFO("column " << c << ": measured " << p.measuredDeg[c] << " deg, confidence " << p.confidence[c]);
        // Measured, not unmeasured: a quarter of the window is co-visible.
        REQUIRE(std::isfinite(p.measuredDeg[c]));
        // The unshifted match is among the candidates (a floor relative to
        // the column's own support), so it is found - not a one-sided shift
        // that slides one lens's covered rows over the other's.
        CHECK(std::fabs(p.measuredDeg[c]) < 0.2f);
        // A quarter of the evidence: next to no weight.
        CHECK(p.confidence[c] < 0.05f);
        CHECK(std::fabs(p.shiftDeg[c]) < 0.05f);
    }
    // Fully covered columns either side are trusted.
    CHECK(p.confidence[700] >= 0.9f);
    CHECK(p.confidence[1300] >= 0.9f);
}

TEST_CASE("a confident change between two anchors still steps, noise glides", "[render][seam][seamtable][temporal]") {
    // Anchor A: textured content at zero disparity.  Anchor B, eight frames
    // later: a near object now covers 900-1099 at +2 deg (the 0.5.0 night
    // traffic-light pole).  Both measurements are confident there, so the
    // glide must put the new table on screen at the anchor; the same change
    // with an unsure measurement is noise and glides.
    const Scene scene(23);
    std::vector<double> disp(kBandW, 0.0);
    const render::SeamProfile a = estimate(sceneBands(scene, disp));
    for (std::uint32_t c = 900; c < 1100; ++c) {
        disp[c] = 2.0 / kDegPerRow;
    }
    const render::SeamProfile bp = estimate(sceneBands(scene, disp));
    INFO("anchor A: T(1000) " << a.shiftDeg[1000] << " conf " << a.confidence[1000] << "; anchor B: T(1000) "
                              << bp.shiftDeg[1000] << " conf " << bp.confidence[1000]);
    REQUIRE(a.confidence[1000] >= 0.6f);
    REQUIRE(bp.confidence[1000] >= 0.6f);
    REQUIRE(std::fabs(a.shiftDeg[1000]) < 0.05f);
    REQUIRE(bp.shiftDeg[1000] == Catch::Approx(2.0).margin(0.1));

    const double t = 1.0 / 8.0;  // the first frame of the bucket
    std::vector<float> out;
    render::blendSeamTables(&a.shiftDeg, &bp.shiftDeg, t, out, render::kSeamTableGlideNoiseDeg,
                            render::kSeamTableStepDeg, &a.confidence, &bp.confidence);
    REQUIRE(out.size() == kBandW);
    // Confident: the new table at once.
    CHECK(out[1000] == Catch::Approx(bp.shiftDeg[1000]).margin(1e-5));
    // Where nothing changed the glide is the plain one (and ~0 here).
    CHECK(out[300] == Catch::Approx(a.shiftDeg[300] + (bp.shiftDeg[300] - a.shiftDeg[300]) * t).margin(1e-5));

    // The same change measured without confidence is matching noise: a glide.
    std::vector<float> unsure = bp.confidence;
    for (float& c : unsure) {
        c *= 0.1f;
    }
    render::blendSeamTables(&a.shiftDeg, &bp.shiftDeg, t, out, render::kSeamTableGlideNoiseDeg,
                            render::kSeamTableStepDeg, &a.confidence, &unsure);
    CHECK(out[1000] == Catch::Approx(a.shiftDeg[1000] + (bp.shiftDeg[1000] - a.shiftDeg[1000]) * t).margin(1e-5));
}

TEST_CASE("the seam-table estimator is defensive and deterministic", "[render][seam][seamtable]") {
    const Scene scene(31);
    render::LensBands b = sceneBands(scene, std::vector<double>(kBandW, 0.0));
    makeSky(b, 600, 1200);

    SECTION("the same table with and without a pool") {
        auto unpooled = render::searchSeamFromBands(b, render::SeamSearchParams{}, nullptr);
        REQUIRE(unpooled.ok());
        const render::SeamProfile pooled = estimate(b);
        REQUIRE(std::memcmp(unpooled.value().shiftDeg.data(), pooled.shiftDeg.data(), kBandW * sizeof(float)) == 0);
        REQUIRE(std::memcmp(unpooled.value().confidence.data(), pooled.confidence.data(), kBandW * sizeof(float)) ==
                0);
    }
    SECTION("a prior is what a featureless stretch settles on; a wrong-size prior is ignored") {
        const std::vector<float> prior(kBandW, 0.7f);
        const render::SeamProfile p = estimate(b, {}, &prior);
        // Mid-sky, 300 columns from the nearest trusted measurement.
        CHECK(p.shiftDeg[900] == Catch::Approx(0.7).margin(0.05));
        // Trusted columns keep their measurement (0) against the weak prior.
        CHECK(std::fabs(p.shiftDeg[300]) < 0.02f);
        const std::vector<float> wrong(kBandW / 2, 0.7f);
        const render::SeamProfile ignored = estimate(b, {}, &wrong);
        const render::SeamProfile none = estimate(b);
        CHECK(std::memcmp(ignored.shiftDeg.data(), none.shiftDeg.data(), kBandW * sizeof(float)) == 0);
        // A non-finite prior entry means "no prior here" (0).
        std::vector<float> holes = prior;
        for (std::uint32_t c = 800; c < 1000; ++c) {
            holes[c] = std::numeric_limits<float>::quiet_NaN();
        }
        const render::SeamProfile h = estimate(b, {}, &holes);
        for (const float v : h.shiftDeg) {
            REQUIRE(std::isfinite(v));
        }
        CHECK(h.shiftDeg[900] < p.shiftDeg[900]);
    }
    SECTION("broken pixels never reach the table") {
        render::LensBands broken = b;
        for (std::size_t i = 0; i < broken.luma[0].size(); i += 97) {
            broken.luma[i % 2][i] = std::numeric_limits<float>::quiet_NaN();
        }
        const render::SeamProfile p = estimate(broken);
        for (std::uint32_t c = 0; c < kBandW; ++c) {
            REQUIRE(std::isfinite(p.shiftDeg[c]));
            REQUIRE(p.confidence[c] >= 0.0f);
            REQUIRE(p.confidence[c] <= 1.0f);
        }
    }
    SECTION("malformed bands are refused") {
        render::LensBands bad = b;
        bad.alpha[1].pop_back();
        CHECK_FALSE(render::searchSeamFromBands(bad, render::SeamSearchParams{}).ok());
        bad = b;
        bad.mapH = 0;
        CHECK_FALSE(render::searchSeamFromBands(bad, render::SeamSearchParams{}).ok());
        render::SeamSearchParams tooFar;
        tooFar.maxShiftPx = 58;  // no matched rows left between the paddings
        CHECK_FALSE(render::searchSeamFromBands(b, tooFar).ok());
        CHECK_FALSE(render::searchSeamFromBands(render::LensBands{}, render::SeamSearchParams{}).ok());
    }
    SECTION("malformed parameters are refused") {
        const auto refused = [&](const render::SeamSearchParams& sp) {
            return !render::searchSeamFromBands(b, sp).ok();
        };
        render::SeamSearchParams sp;
        sp.confNccHi = sp.confNccLo;  // not rising
        CHECK(refused(sp));
        sp = {};
        sp.confTextureLo = std::numeric_limits<double>::quiet_NaN();
        CHECK(refused(sp));
        sp = {};
        sp.huberDeg = 0.0;
        CHECK(refused(sp));
        sp = {};
        sp.irlsIterations = 0;
        CHECK(refused(sp));
        sp = {};
        sp.priorWeight = 0.0;
        CHECK(refused(sp));
        sp = {};
        sp.unmeasuredPriorWeight = std::numeric_limits<double>::quiet_NaN();
        CHECK(refused(sp));
        sp = {};
        sp.smoothLambda2 = -1.0;
        CHECK(refused(sp));
        sp = {};
        sp.minCovalidFraction = 1.5;
        CHECK(refused(sp));
        sp = {};
        sp.windowHalfCols = 1100;  // wider than the ring
        CHECK(refused(sp));
    }
}
