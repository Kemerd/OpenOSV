// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Seam regression tests on the sample clip: the verified conventions must
// give a well aligned overlap band, and the alternatives must be measurably
// worse.  This is the test that catches a wrong sign anywhere in the
// geometry chain.

#include <catch2/catch_test_macros.hpp>

#include "TestSample.h"

#include "osv/container/OsvFile.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/DualStreamReader.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

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

    // Pure 0.78125 scale (no crop) must be measurably worse.
    auto rigScale = buildRig(l.value(), 3000.0 / 3840.0);
    REQUIRE(rigScale.ok());
    auto nccScale = render::overlapNcc(rigScale.value(), l.value().pair, blend, band, pool);
    REQUIRE(nccScale.ok());
    INFO("NCC with 0.78125 scale: " << nccScale.value());
    // The feathered 4 deg band is only mildly sensitive to the crop scale on this
    // clip (measured 0.870 vs 0.856); the verified value must still win.
    REQUIRE(ncc.value() > nccScale.value());

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
