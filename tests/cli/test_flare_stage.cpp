// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_flare_stage.cpp - the plug-in engine's sun ghost stage
// (plugins/importer/FlareStage.h) on its own, without a host.
//
// FlareStage is SDK-free (osvtool and the OpenFX bundle compile it as-is), so
// it is driven here directly with a decoded frame of the sample clip.  What
// is proven:
//
//   * the clip's EV100 is read from what the camera recorded (ISO, shutter,
//     aperture), and is unknown - NaN - when the metadata cannot say;
//   * a frame metered too dark for the sun to be in view gets nothing: no
//     model for the builder, no seam penalty, an exact answer - whatever the
//     stage measured for that frame before;
//   * an unknown or a daylight EV100 leaves the stage exactly as it was: the
//     sample's ghosts are measured and handed over;
//   * the dark answer comes before the sun check: a pair the check would
//     refuse is answered without complaint.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "FlareStage.h"

#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/Flare.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/DualStreamReader.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace osv;
using Catch::Matchers::WithinAbs;
using premiere::FlareStage;

namespace {

/// The sample's frame 3 (two ghosts in the master lens) and everything the
/// stage needs to look at it, built the way the importer builds it.
struct SampleFrame {
    std::unique_ptr<OsvFile> file;
    meta::MetadataTrack track;
    geom::LensRig rig;
    video::FramePair pair;
};

/// The frame the verification log names: 'master lens: sun 10.0 deg off
/// axis, 2 ghosts removed (+23% at (1183, 1548), +7% at (833, 1290))'.
constexpr std::uint32_t kFrame = 3;

[[nodiscard]] Result<SampleFrame> openSample(std::uint32_t index) {
    SampleFrame s;
    OSV_TRY_ASSIGN(OsvFile f, OsvFile::open(osvtest::sampleOsv()));
    s.file = std::make_unique<OsvFile>(std::move(f));
    OSV_TRY_ASSIGN(s.track, meta::MetadataTrack::load(*s.file));
    OSV_TRY_ASSIGN(meta::FormatInfo format, meta::FormatDetector::detect(*s.file, &s.track));
    OSV_TRY_ASSIGN(meta::CalibrationSet cal, meta::CalibrationSelector::select(s.track.stream()));
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(static_cast<int>(format.streamW), static_cast<int>(format.streamH),
                                               static_cast<int>(format.sensorW), static_cast<int>(format.sensorH),
                                               format.digitalFocalLength, 0.5 * (cal.slave.fx + cal.master.fx)));
    OSV_TRY_ASSIGN(s.rig, geom::LensRig::build(cal, scaling, geom::FocalSource::DigitalFocalLength,
                                               format.digitalFocalLength, geom::ExtrinsicConvention{}));
    OSV_TRY_ASSIGN(video::DualStreamReader reader, video::DualStreamReader::open(osvtest::sampleOsv(), format));
    OSV_TRY_ASSIGN(s.pair, reader.read(index));
    return s;
}

/// The colour block the stage reads its input decode from (D-Log M in, a
/// linear-light output, so the removal is not switched off by passthrough).
[[nodiscard]] OsvColorParams pqColor() {
    return color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
}

/// True when the stage's seam penalty would steer a carve: the hook answers
/// for a band of a size it accepts only while a model with a sun is held.
[[nodiscard]] bool seamPenaltyActive(FlareStage& stage) {
    render::LensBands bands;
    bands.w = 64;
    bands.h = 8;
    bands.mapH = 512;
    bands.rowOffset = 252;
    std::vector<float> slave(static_cast<std::size_t>(bands.w) * bands.h, 0.0f);
    std::vector<float> master(slave.size(), 0.0f);
    const render::SeamPenaltyHook hook = stage.seamPenalty();
    REQUIRE(hook.installed());
    return hook.fn(bands, slave, master, hook.user);
}

/// One Exact, non-draft request of frame `index` with removal on.
[[nodiscard]] FlareStage::Outcome applyExact(FlareStage& stage, std::uint32_t index, const SampleFrame& s,
                                             double sceneEv100, ThreadPool& pool) {
    render::RenderParamsBuilder builder;
    return stage.apply(index, s.pair, s.rig, pqColor(), sceneEv100, /*enabled=*/true, /*draft=*/false,
                       /*exactWanted=*/true, pool, builder, "sample");
}

}  // namespace

TEST_CASE("the flare stage reads the scene's EV100 from the camera's own metadata", "[flare][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto file = OsvFile::open(osvtest::sampleOsv());
    REQUIRE(file.ok());
    auto track = meta::MetadataTrack::load(file.value());
    REQUIRE(track.ok());
    // ISO 142 at 1/208 s and f/1.9: the sun through an ND filter, 3 stops
    // above the gate.
    const double ev = FlareStage::sceneEv100(track.value(), kFrame);
    INFO("EV100 " << ev);
    REQUIRE_THAT(ev, WithinAbs(9.05, 0.02));
    REQUIRE_FALSE(render::flareSceneTooDark(ev, render::FlareParams{}));
    // A frame past the end of the clip has no metadata: unknown, not a guess.
    REQUIRE(std::isnan(FlareStage::sceneEv100(track.value(), 1000000u)));
    // Nor does a track that was never loaded.
    REQUIRE(std::isnan(FlareStage::sceneEv100(meta::MetadataTrack{}, 0)));
}

TEST_CASE("a frame metered too dark for the sun gets nothing, and an unknown brightness changes nothing",
          "[flare][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto opened = openSample(kFrame);
    REQUIRE(opened.ok());
    const SampleFrame& s = opened.value();
    ThreadPool pool;
    const double nan = std::numeric_limits<double>::quiet_NaN();

    SECTION("unknown brightness: the ghosts are measured and handed over, as before") {
        FlareStage stage;
        const FlareStage::Outcome out = applyExact(stage, kFrame, s, nan, pool);
        REQUIRE(out.exact);
        REQUIRE(out.applied);
        REQUIRE(seamPenaltyActive(stage));
    }

    SECTION("daylight brightness (the clip's own 9.05): the same") {
        FlareStage stage;
        const FlareStage::Outcome out = applyExact(stage, kFrame, s, 9.05, pool);
        REQUIRE(out.exact);
        REQUIRE(out.applied);
        REQUIRE(seamPenaltyActive(stage));
        // Exactly at the threshold is not below it.
        FlareStage atGate;
        REQUIRE(applyExact(atGate, kFrame, s, render::FlareParams{}.minSceneEv100, pool).applied);
    }

    SECTION("night brightness: nothing for the builder, no seam penalty, an exact frame") {
        FlareStage stage;
        render::RenderParamsBuilder builder;
        const FlareStage::Outcome out = stage.apply(kFrame, s.pair, s.rig, pqColor(), 3.3, true, false, true, pool,
                                                    builder, "sample");
        REQUIRE(out.exact);
        REQUIRE_FALSE(out.applied);
        REQUIRE_FALSE(seamPenaltyActive(stage));
        // An Interactive request is just as final: nothing is queued for it.
        const FlareStage::Outcome interactive =
            stage.apply(kFrame, s.pair, s.rig, pqColor(), 3.3, true, false, false, pool, builder, "sample");
        REQUIRE(interactive.exact);
        REQUIRE_FALSE(interactive.applied);
    }

    SECTION("the gate wins over a model measured for the same frame earlier") {
        FlareStage stage;
        REQUIRE(applyExact(stage, kFrame, s, nan, pool).applied);
        REQUIRE(seamPenaltyActive(stage));
        // Same frame, now known to be dark: the cached answer is not reached
        // and the penalty the earlier frame installed is cleared.
        const FlareStage::Outcome dark = applyExact(stage, kFrame, s, 2.5, pool);
        REQUIRE(dark.exact);
        REQUIRE_FALSE(dark.applied);
        REQUIRE_FALSE(seamPenaltyActive(stage));
        // And the frame's own answer is still there for a known daylight value.
        REQUIRE(applyExact(stage, kFrame, s, 9.05, pool).applied);
    }
}

TEST_CASE("the dark answer comes before any image is looked at", "[flare]") {
    // A pair with no frames in it: the sun check would refuse it (and log
    // the failure); a dark frame never gets that far.
    FlareStage stage;
    ThreadPool pool;
    render::RenderParamsBuilder builder;
    const video::FramePair empty{};
    const geom::LensRig rig{};
    const FlareStage::Outcome dark =
        stage.apply(0, empty, rig, pqColor(), 2.0, true, false, true, pool, builder, "synthetic");
    REQUIRE(dark.exact);
    REQUIRE_FALSE(dark.applied);
    REQUIRE_FALSE(seamPenaltyActive(stage));
    // Unknown brightness takes the old road: the check fails, the frame goes
    // without removal - never an exception, never a crash.
    const FlareStage::Outcome unknown = stage.apply(0, empty, rig, pqColor(), std::nan(""), true, false, true, pool,
                                                    builder, "synthetic");
    REQUIRE_FALSE(unknown.applied);
    // Off in Source Settings and draft requests are unchanged by the gate.
    REQUIRE_FALSE(stage.apply(0, empty, rig, pqColor(), 2.0, false, false, true, pool, builder, "synthetic").applied);
    REQUIRE_FALSE(stage.apply(0, empty, rig, pqColor(), 2.0, true, true, true, pool, builder, "synthetic").applied);
}
