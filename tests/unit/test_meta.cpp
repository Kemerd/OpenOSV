// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Sample-clip tests for osv_meta: the typed decode of djmd sample 0 and of
// frames 0/1/64 against the goldens written by scripts/gen_golden.py
// (an independent pure-Python decoder), the whole-track invariants, the
// primary track selection, FormatDetector, CalibrationSelector and the LRF
// proxy.  Every test here SKIPs when the clip is not on the machine.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/container/OsvFile.h"
#include "osv/core/ThreadPool.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/DjmdDecoder.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetaJson.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/meta/ProtoTree.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::meta;
using nlohmann::json;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// Recursive comparison of a library JSON document against a golden one.
/// Every key of the golden must exist in `actual` with an equal value:
/// strings, bools and integers exactly, floating point numbers to a
/// relative 1e-6 (the goldens are float32 values widened by Python exactly
/// as MetaJson widens them, so this is generous).  Extra keys in `actual`
/// are allowed (the library reports more than the generator).
void requireJsonMatches(const json& actual, const json& golden, const std::string& path) {
    INFO("at " << path);
    if (golden.is_null()) {
        REQUIRE(actual.is_null());
        return;
    }
    if (golden.is_object()) {
        REQUIRE(actual.is_object());
        for (auto it = golden.begin(); it != golden.end(); ++it) {
            INFO("missing key " << it.key() << " at " << path);
            REQUIRE(actual.contains(it.key()));
            requireJsonMatches(actual.at(it.key()), it.value(), path + "." + it.key());
        }
        return;
    }
    if (golden.is_array()) {
        REQUIRE(actual.is_array());
        REQUIRE(actual.size() == golden.size());
        for (std::size_t i = 0; i < golden.size(); ++i) {
            requireJsonMatches(actual.at(i), golden.at(i), path + "[" + std::to_string(i) + "]");
        }
        return;
    }
    if (golden.is_string()) {
        REQUIRE(actual.is_string());
        REQUIRE(actual.get<std::string>() == golden.get<std::string>());
        return;
    }
    if (golden.is_boolean()) {
        REQUIRE(actual.is_boolean());
        REQUIRE(actual.get<bool>() == golden.get<bool>());
        return;
    }
    if (golden.is_number()) {
        REQUIRE(actual.is_number());
        // Integers on both sides: exact.  Anything floating: relative.
        if (golden.is_number_integer() && actual.is_number_integer()) {
            if (golden.is_number_unsigned() && actual.is_number_unsigned()) {
                REQUIRE(actual.get<std::uint64_t>() == golden.get<std::uint64_t>());
            } else {
                REQUIRE(actual.get<std::int64_t>() == golden.get<std::int64_t>());
            }
        } else {
            const double a = actual.get<double>();
            const double g = golden.get<double>();
            INFO("actual " << a << " golden " << g);
            REQUIRE(osvtest::approxRel(a, g, 1e-6, 1e-9));
        }
        return;
    }
    FAIL("unsupported golden value type at " << path);
}

/// Open the sample clip (the caller has already checked it exists).
OsvFile openSample() {
    Result<OsvFile> file = OsvFile::open(osvtest::sampleOsv());
    REQUIRE(file.ok());
    return std::move(file).value();
}

/// True when the LRF proxy next to the sample exists.
bool haveLrf() {
    std::error_code ec;
    return std::filesystem::exists(osvtest::sampleLrf(), ec) && !ec;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Sample 0 vs golden
// -----------------------------------------------------------------------------

TEST_CASE("MetadataTrack loads the primary djmd track of the sample", "[meta][sample]") {
    OSV_REQUIRE_SAMPLE();
    const OsvFile file = openSample();
    const json golden = osvtest::loadGolden("sample_probe.json");

    Result<MetadataTrack> loaded = MetadataTrack::load(file);
    REQUIRE(loaded.ok());
    const MetadataTrack& track = loaded.value();
    REQUIRE(track.trackId() == golden.at("metaTrackId").get<std::uint32_t>());
    REQUIRE(track.trackId() == 4);
    REQUIRE(track.frameCount() == golden.at("frameCount").get<std::uint32_t>());
    REQUIRE(track.frameCount() == 65);
    REQUIRE(track.hasClip());
    REQUIRE(track.hasStream());
    REQUIRE(track.hasCalibration());
    REQUIRE(track.file() == &file);
    // Nothing in the sample should trip a warning.
    for (const std::string& w : track.warnings()) {
        INFO(w);
    }
    REQUIRE(track.warnings().empty());
}

TEST_CASE("ClipMeta of sample 0 equals the golden", "[meta][sample]") {
    OSV_REQUIRE_SAMPLE();
    const OsvFile file = openSample();
    const json golden = osvtest::loadGolden("sample_probe.json");
    Result<MetadataTrack> loaded = MetadataTrack::load(file);
    REQUIRE(loaded.ok());
    const ClipMeta& c = loaded.value().clip();

    // Spot checks with the published values first (readable failures)...
    REQUIRE(c.header.protoFileName == "dvtm_oq101.proto");
    REQUIRE(c.header.libVersion == "02.01.15");
    REQUIRE(c.header.productProtoVersion == "2.0.8");
    REQUIRE(c.header.serialNumber == "95SXN7X0221LX4");
    REQUIRE(c.header.firmware == "10.00.25.29");
    REQUIRE(c.header.clipTimestampUs == 30669420766ull);
    REQUIRE(c.header.productName == "Osmo 360");
    REQUIRE(c.videoStreamCount == 1);
    REQUIRE(c.audioStreamCount == 1);
    REQUIRE(c.distortionCoefficients.size() == 4);
    REQUIRE_THAT(c.distortionCoefficients[0], WithinRel(0.1551311f, 1e-5f));
    REQUIRE_THAT(c.distortionCoefficients[3], WithinRel(0.0041704f, 1e-4f));
    REQUIRE(c.sensorReadoutTime == 17282160);
    REQUIRE(c.sensorReadDirection == 4);
    REQUIRE_THAT(c.digitalFocalLength, WithinRel(829.3612f, 1e-6f));
    REQUIRE(c.eisStatus == EisStatus::Off);
    REQUIRE(c.imuSamplingRate == 1000);
    REQUIRE_THAT(c.sensorFps, WithinRel(59.939388f, 1e-6f));
    REQUIRE(c.itd == -136);
    REQUIRE(c.lro == 3429);
    REQUIRE(c.sensorW == 3840);
    REQUIRE(c.sensorH == 3840);
    REQUIRE(c.fNumber == std::vector<std::uint32_t>{19, 10});
    // ...then the whole document against the independent decoder.
    requireJsonMatches(toJson(c), golden.at("clip"), "clip");
}

TEST_CASE("StreamMeta of sample 0 equals the golden including all calibration slots", "[meta][sample]") {
    OSV_REQUIRE_SAMPLE();
    const OsvFile file = openSample();
    const json golden = osvtest::loadGolden("sample_probe.json");
    Result<MetadataTrack> loaded = MetadataTrack::load(file);
    REQUIRE(loaded.ok());
    const StreamMeta& s = loaded.value().stream();

    REQUIRE(s.id == 0);
    REQUIRE(s.type == 0);
    REQUIRE(s.name == "video");
    REQUIRE(s.video.width == 3000);
    REQUIRE(s.video.height == 3000);
    REQUIRE_THAT(s.video.fps, WithinRel(59.94f, 1e-5f));
    REQUIRE(s.video.bitDepthValid);
    REQUIRE(s.video.bitDepth == 10);
    REQUIRE(s.video.bitFormat == 4);
    REQUIRE(s.video.codec == 1);
    REQUIRE(s.colorMode == ColorMode::DLogM);
    REQUIRE(s.fovType == 3);
    REQUIRE(s.extriLensMode == ExtriLensMode::Native);
    REQUIRE(s.shadingCalibModeNum == 7);

    // Slot population: 1/2, 11/12 and the six far pairs; 3-10 empty.
    for (std::uint32_t slot = 1; slot < PanoDewarpParams::SlotCount; ++slot) {
        INFO("slot " << slot << " " << PanoDewarpParams::fieldName(slot));
        const bool expectPopulated = slot <= 2 || slot >= 11;
        REQUIRE((s.dewarp.get(slot) != nullptr) == expectPopulated);
        if (expectPopulated) {
            REQUIRE(s.dewarp.get(slot)->hasCore());
            REQUIRE(s.dewarp.get(slot)->width == 3840);
            REQUIRE(s.dewarp.get(slot)->height == 3840);
        }
    }
    // The refined slave record with the published numbers.
    const DewarpParams* slave = s.dewarp.get(PanoDewarpParams::NativeRefineSlave);
    REQUIRE(slave != nullptr);
    REQUIRE_THAT(slave->fx, WithinRel(1043.8802f, 1e-6f));
    REQUIRE_THAT(slave->fy, WithinRel(1043.6731f, 1e-6f));
    REQUIRE_THAT(slave->cx, WithinRel(1917.0421f, 1e-6f));
    REQUIRE_THAT(slave->cy, WithinRel(1919.1294f, 1e-6f));
    REQUIRE_THAT(slave->k[0], WithinRel(0.0667397f, 1e-5f));
    REQUIRE_THAT(slave->k[4], WithinRel(0.00098791f, 1e-4f));
    REQUIRE(slave->k[5] == 0.0f);
    REQUIRE_THAT(slave->yaw, WithinRel(-179.62978f, 1e-5f));
    REQUIRE(slave->occlusionPtX.size() == 14);
    REQUIRE(slave->occlusionPtY.size() == 14);
    REQUIRE_THAT(slave->occlusionPtX[0], WithinAbs(1920.0f, 1e-3f));
    REQUIRE_THAT(slave->occlusionPtY[0], WithinAbs(3735.0f, 1e-3f));
    REQUIRE(slave->q.size() == 4);
    REQUIRE(slave->camExtriQ.present);
    REQUIRE_THAT(slave->camExtriQ.w, WithinRel(0.0026615f, 1e-4f));
    REQUIRE_THAT(slave->camExtriQ.y, WithinRel(-0.7056412f, 1e-6f));
    REQUIRE(slave->camExtriQ.w == slave->q[0]);
    REQUIRE(slave->camExtriQ.z == slave->q[3]);
    REQUIRE(slave->lensModel == 8.0f);
    REQUIRE(slave->temperature == -1000.0f);
    REQUIRE_FALSE(slave->camImuExtriQ.present);

    const DewarpParams* master = s.dewarp.get(PanoDewarpParams::NativeRefineMaster);
    REQUIRE(master != nullptr);
    REQUIRE_THAT(master->fx, WithinRel(1043.0103f, 1e-6f));
    REQUIRE_THAT(master->camExtriQ.w, WithinRel(0.7036960f, 1e-6f));
    REQUIRE_THAT(master->camExtriQ.x, WithinRel(0.7103991f, 1e-6f));

    // Raw native pair and the far presets from the facts table.
    REQUIRE_THAT(s.dewarp.get(PanoDewarpParams::NativeSlave)->fx, WithinRel(1042.22f, 1e-5f));
    REQUIRE_THAT(s.dewarp.get(PanoDewarpParams::NativeMaster)->fx, WithinRel(1037.85f, 1e-5f));
    REQUIRE_THAT(s.dewarp.get(PanoDewarpParams::Far07Slave)->fx, WithinRel(1042.24f, 1e-5f));
    REQUIRE_THAT(s.dewarp.get(PanoDewarpParams::Far16Master)->fx, WithinRel(1039.39f, 1e-5f));

    requireJsonMatches(toJson(s), golden.at("stream"), "stream");
}

TEST_CASE("Raw tree of sample 0 shows the three top-level messages", "[meta][sample]") {
    OSV_REQUIRE_SAMPLE();
    const OsvFile file = openSample();
    Result<ByteSpan> sample0 = file.sample(4, 0);
    REQUIRE(sample0.ok());
    REQUIRE(sample0.value().size() == 6381);
    const ProtoTree tree = decodeTree(sample0.value());
    REQUIRE_FALSE(tree.failed);
    REQUIRE(tree.fields.size() == 3);
    REQUIRE(tree.fields[0].number == 1);
    REQUIRE(tree.fields[1].number == 2);
    REQUIRE(tree.fields[2].number == 3);
    REQUIRE(tree.fields[1].isMessage);
    // The pano_dewarp_params message carries all 24 slots.
    const ProtoNode* pano = findChild(tree.fields[1].children, 6);
    REQUIRE(pano != nullptr);
    REQUIRE(pano->isMessage);
    REQUIRE(pano->children.size() == 24);
    REQUIRE(tree.nodeCount > 200);
    // Serialisation round-trips without throwing and stays a document.
    const json j = toJson(tree);
    REQUIRE(j.at("fields").size() == 3);
}

// -----------------------------------------------------------------------------
//  Frames
// -----------------------------------------------------------------------------

TEST_CASE("Frames 0, 1 and 64 equal the golden", "[meta][sample]") {
    OSV_REQUIRE_SAMPLE();
    const OsvFile file = openSample();
    const json golden = osvtest::loadGolden("sample_frames_0_1_64.json");
    Result<MetadataTrack> loaded = MetadataTrack::load(file);
    REQUIRE(loaded.ok());
    const MetadataTrack& track = loaded.value();
    REQUIRE(track.trackId() == golden.at("metaTrackId").get<std::uint32_t>());

    for (const char* key : {"0", "1", "64"}) {
        const std::uint32_t index = static_cast<std::uint32_t>(std::stoul(key));
        Result<FrameMeta> frame = track.frame(index);
        REQUIRE(frame.ok());
        requireJsonMatches(toJson(frame.value()), golden.at("frames").at(key), std::string("frames.") + key);
    }
    // Published frame-0 values.
    Result<FrameMeta> f0 = track.frame(0);
    REQUIRE(f0.ok());
    REQUIRE(f0.value().seq == 0);
    REQUIRE(f0.value().timestampUs == 30669420766ull);
    REQUIRE(f0.value().streamId == 0);
    REQUIRE(f0.value().camera.iso == 142.0f);
    REQUIRE(f0.value().camera.exposureTime == std::vector<std::int32_t>{1, 208});
    REQUIRE(f0.value().camera.wbCct == 6545);
    REQUIRE_THAT(f0.value().camera.attitude.w, WithinRel(0.46131432f, 1e-6f));
    REQUIRE_THAT(f0.value().camera.attitude.x, WithinRel(0.54081959f, 1e-6f));
    REQUIRE_THAT(f0.value().camera.attitude.y, WithinRel(0.54205739f, 1e-6f));
    REQUIRE_THAT(f0.value().camera.attitude.z, WithinRel(0.44819313f, 1e-6f));
    REQUIRE(f0.value().imu.has_value());
    REQUIRE(f0.value().imu->current.ts == 604649192);
    REQUIRE(f0.value().imu->current.vsync == 238617);
    REQUIRE_THAT(f0.value().imu->current.offset, WithinRel(-0.9025f, 1e-4f));
    REQUIRE(f0.value().imu->vsyncPos == 5);
    REQUIRE(f0.value().gimbalDeviceName == "Osmo OQ001");
    REQUIRE_THAT(f0.value().gimbalDeviceFrequency, WithinRel(59.94f, 1e-5f));
    // Frame 64 is the last one: seq 128.
    Result<FrameMeta> f64 = track.frame(64);
    REQUIRE(f64.ok());
    REQUIRE(f64.value().seq == 128);
    // Out of range.
    REQUIRE(track.frame(65).code() == ErrorCode::NotFound);
    REQUIRE(track.frame(UINT32_MAX).code() == ErrorCode::NotFound);
}

TEST_CASE("Whole-track invariants hold on every frame", "[meta][sample]") {
    OSV_REQUIRE_SAMPLE();
    const OsvFile file = openSample();
    const json golden = osvtest::loadGolden("sample_frames_0_1_64.json");
    Result<MetadataTrack> loaded = MetadataTrack::load(file);
    REQUIRE(loaded.ok());
    const MetadataTrack& track = loaded.value();
    REQUIRE(track.frameCount() == 65);

    // Fill the cache in parallel first; every later frame() call is a hit.
    ThreadPool pool(4);
    REQUIRE(track.prefetchAll(&pool).ok());

    std::uint64_t previousTs = 0;
    std::uint32_t previousSeq = 0;
    std::uint32_t previousVsync = 0;
    for (std::uint32_t i = 0; i < track.frameCount(); ++i) {
        INFO("frame " << i);
        Result<FrameMeta> frame = track.frame(i);
        REQUIRE(frame.ok());
        const FrameMeta& f = frame.value();
        REQUIRE(f.streamId == 0);
        // Sequence numbers advance by two per frame.
        REQUIRE(f.seq == 2 * i);
        if (i > 0) {
            REQUIRE(f.seq == previousSeq + 2);
            REQUIRE(f.timestampUs > previousTs);
            // ~16683.5 us per frame at 59.94 fps.
            const std::uint64_t delta = f.timestampUs - previousTs;
            REQUIRE(delta >= 16600);
            REQUIRE(delta <= 16800);
        }
        previousSeq = f.seq;
        previousTs = f.timestampUs;

        // IMU batch of 16 or 17 samples whose 5th entry is the camera attitude.
        REQUIRE(f.imu.has_value());
        const ImuBatch& batch = f.imu->current;
        REQUIRE(batch.present);
        REQUIRE(batch.q.size() >= 16);
        REQUIRE(batch.q.size() <= 17);
        REQUIRE(f.camera.attitude.present);
        REQUIRE_THAT(batch.q[4].w, WithinAbs(f.camera.attitude.w, 1e-6f));
        REQUIRE_THAT(batch.q[4].x, WithinAbs(f.camera.attitude.x, 1e-6f));
        REQUIRE_THAT(batch.q[4].y, WithinAbs(f.camera.attitude.y, 1e-6f));
        REQUIRE_THAT(batch.q[4].z, WithinAbs(f.camera.attitude.z, 1e-6f));
        // Unit quaternions throughout.
        for (const Quaternion& q : batch.q) {
            const double n = std::sqrt(static_cast<double>(q.w) * q.w + static_cast<double>(q.x) * q.x +
                                       static_cast<double>(q.y) * q.y + static_cast<double>(q.z) * q.z);
            REQUIRE_THAT(n, WithinAbs(1.0, 1e-3));
        }
        // The sensor frame counter advances by one per frame.
        if (i > 0) {
            REQUIRE(batch.vsync == previousVsync + 1);
        }
        previousVsync = batch.vsync;
        REQUIRE(f.imu->vsyncPos <= 7);
        REQUIRE_FALSE(f.imu->prev.present);
        REQUIRE_FALSE(f.imu->next.present);
    }
    // The invariants recorded by the generator agree.
    const json inv = golden.at("invariants");
    REQUIRE(inv.at("seqSteps") == json::array({2}));
    REQUIRE(inv.at("imuBatchSizes") == json::array({16, 17}));
    REQUIRE(inv.at("attitudeEqualsImuAnchor4").get<bool>());
    REQUIRE(inv.at("frameCount").get<std::uint32_t>() == track.frameCount());
}

TEST_CASE("prefetchAll works without a pool and is idempotent", "[meta][sample]") {
    OSV_REQUIRE_SAMPLE();
    const OsvFile file = openSample();
    Result<MetadataTrack> loaded = MetadataTrack::load(file);
    REQUIRE(loaded.ok());
    REQUIRE(loaded.value().prefetchAll(nullptr).ok());
    REQUIRE(loaded.value().prefetchAll(nullptr).ok());
    REQUIRE(loaded.value().frame(33).ok());
    // A default constructed track is not loaded and says so.
    MetadataTrack empty;
    REQUIRE(empty.frame(0).code() == ErrorCode::Internal);
    REQUIRE(empty.prefetchAll().code() == ErrorCode::Internal);
    REQUIRE(empty.lensStreamOrder().code() == ErrorCode::Internal);
    REQUIRE(empty.frameCount() == 0);
    REQUIRE_FALSE(empty.hasClip());
    REQUIRE(empty.clip().header.productName.empty());
}

// -----------------------------------------------------------------------------
//  Track selection
// -----------------------------------------------------------------------------

TEST_CASE("The secondary djmd track loads but carries no calibration or IMU", "[meta][sample]") {
    OSV_REQUIRE_SAMPLE();
    const OsvFile file = openSample();
    Result<MetadataTrack> loaded = MetadataTrack::load(file, 5u);
    REQUIRE(loaded.ok());
    const MetadataTrack& track = loaded.value();
    REQUIRE(track.trackId() == 5);
    REQUIRE(track.frameCount() == 65);
    // Same headers as track 4...
    REQUIRE(track.hasClip());
    REQUIRE(track.hasStream());
    REQUIRE(track.clip().header.serialNumber == "95SXN7X0221LX4");
    REQUIRE(track.stream().colorMode == ColorMode::DLogM);
    // ...but no calibration at all, so the selector has nothing to pick.
    REQUIRE_FALSE(track.hasCalibration());
    for (std::uint32_t slot = 1; slot < PanoDewarpParams::SlotCount; ++slot) {
        REQUIRE(track.stream().dewarp.get(slot) == nullptr);
    }
    std::vector<std::string> warnings;
    const Result<CalibrationSet> selected = CalibrationSelector::select(track.stream(), {}, &warnings);
    REQUIRE_FALSE(selected.ok());
    REQUIRE(selected.code() == ErrorCode::NotFound);
    // Frames decode, with the camera attitude but without an IMU batch.
    Result<FrameMeta> frame = track.frame(1);
    REQUIRE(frame.ok());
    REQUIRE(frame.value().seq == 2);
    REQUIRE(frame.value().camera.attitude.present);
    REQUIRE((!frame.value().imu.has_value() || !frame.value().imu->current.present));

    // Explicit requests for non-djmd or missing tracks fail cleanly.
    REQUIRE(MetadataTrack::load(file, 1u).code() == ErrorCode::InvalidArgument);
    REQUIRE(MetadataTrack::load(file, 3u).code() == ErrorCode::InvalidArgument);
    REQUIRE(MetadataTrack::load(file, 99u).code() == ErrorCode::NotFound);
}

TEST_CASE("lensStreamOrder maps track 1 to the slave and track 2 to the master", "[meta][sample]") {
    OSV_REQUIRE_SAMPLE();
    const OsvFile file = openSample();
    Result<MetadataTrack> loaded = MetadataTrack::load(file);
    REQUIRE(loaded.ok());
    Result<MetadataTrack::LensStreamOrder> order = loaded.value().lensStreamOrder();
    REQUIRE(order.ok());
    REQUIRE(order.value().slaveTrackId == 1);
    REQUIRE(order.value().masterTrackId == 2);
    REQUIRE_FALSE(order.value().sideBySide);
    // The camera writes tkhd flags 0x3 / 0x2, so this is not the fallback.
    REQUIRE_FALSE(order.value().fromFallback);
    REQUIRE(order.value().source.find("tkhd") != std::string::npos);
    REQUIRE(order.value().source.find("stream id 0") != std::string::npos);
    // The static form without metadata gives the same answer.
    Result<MetadataTrack::LensStreamOrder> bare = MetadataTrack::lensStreamOrderFor(file, nullptr);
    REQUIRE(bare.ok());
    REQUIRE(bare.value().slaveTrackId == 1);
    REQUIRE(bare.value().masterTrackId == 2);
}

// -----------------------------------------------------------------------------
//  FormatDetector
// -----------------------------------------------------------------------------

TEST_CASE("FormatDetector recognises the 6K dual-fisheye D-Log M clip", "[meta][sample]") {
    OSV_REQUIRE_SAMPLE();
    const OsvFile file = openSample();
    const json golden = osvtest::loadGolden("sample_probe.json");
    Result<MetadataTrack> loaded = MetadataTrack::load(file);
    REQUIRE(loaded.ok());

    Result<FormatInfo> detected = FormatDetector::detect(file, &loaded.value());
    REQUIRE(detected.ok());
    const FormatInfo& f = detected.value();
    REQUIRE(f.cameraModel == "Osmo 360");
    REQUIRE(f.mode == Mode::K6);
    REQUIRE(std::string(modeName(f.mode)) == "K6");
    REQUIRE(f.streamW == 3000);
    REQUIRE(f.streamH == 3000);
    REQUIRE_THAT(f.fps, WithinAbs(59.94, 0.01));
    REQUIRE(f.colorMode == ColorMode::DLogM);
    REQUIRE(f.colorModeFromMetadata);
    REQUIRE(f.lensMode == ExtriLensMode::Native);
    REQUIRE(f.bitDepth == 10);
    REQUIRE(f.dualFisheye);
    REQUIRE(f.videoTrackIds[0] == 1);
    REQUIRE(f.videoTrackIds[1] == 2);
    REQUIRE(f.metaTrackId == 4);
    REQUIRE(f.sensorW == 3840);
    REQUIRE(f.sensorH == 3840);
    REQUIRE_THAT(f.digitalFocalLength, WithinRel(829.3612f, 1e-6f));
    REQUIRE_FALSE(f.sideBySideProxy);
    // The golden's format block agrees field by field.
    requireJsonMatches(toJson(f), golden.at("format"), "format");

    // Container-only detection still finds the geometry but not the colour mode.
    Result<FormatInfo> bare = FormatDetector::detect(file, nullptr);
    REQUIRE(bare.ok());
    REQUIRE(bare.value().mode == Mode::K6);
    REQUIRE(bare.value().dualFisheye);
    REQUIRE(bare.value().videoTrackIds[0] == 1);
    REQUIRE(bare.value().videoTrackIds[1] == 2);
    REQUIRE(bare.value().colorMode == ColorMode::Unknown);
    REQUIRE_FALSE(bare.value().colorModeFromMetadata);
    REQUIRE(bare.value().bitDepth == 10);  // from hvcC
    REQUIRE(bare.value().cameraModel == "Osmo 360");  // from udta (c)too
    REQUIRE_FALSE(bare.value().notes.empty());

    REQUIRE(FormatDetector::modeFromWidth(1920, false) == Mode::K4);
    REQUIRE(FormatDetector::modeFromWidth(3840, false) == Mode::K8);
    REQUIRE(FormatDetector::modeFromWidth(1234, false) == Mode::Unknown);
    REQUIRE(FormatDetector::modeFromWidth(2048, true) == Mode::Lrf);
}

// -----------------------------------------------------------------------------
//  CalibrationSelector
// -----------------------------------------------------------------------------

TEST_CASE("CalibrationSelector picks the refined native pair by default", "[meta][sample]") {
    OSV_REQUIRE_SAMPLE();
    const OsvFile file = openSample();
    const json golden = osvtest::loadGolden("sample_probe.json");
    Result<MetadataTrack> loaded = MetadataTrack::load(file);
    REQUIRE(loaded.ok());
    const StreamMeta& s = loaded.value().stream();

    std::vector<std::string> warnings;
    Result<CalibrationSet> selected = CalibrationSelector::select(s, {}, &warnings);
    REQUIRE(selected.ok());
    REQUIRE(warnings.empty());
    const CalibrationSet& set = selected.value();
    REQUIRE(set.sourceSlave == "native_refine_slave");
    REQUIRE(set.sourceMaster == "native_refine_master");
    REQUIRE_THAT(set.slave.fx, WithinRel(1043.8802f, 1e-6f));
    REQUIRE_THAT(set.master.fx, WithinRel(1043.0103f, 1e-6f));
    requireJsonMatches(toJson(set.slave), golden.at("stream").at("dewarp").at("native_refine_slave"), "slave");
    requireJsonMatches(toJson(set.master), golden.at("stream").at("dewarp").at("native_refine_master"), "master");
    const json setJson = toJson(set);
    REQUIRE(setJson.at("sourceSlave") == "native_refine_slave");

    // Raw native pair when the refined one is not preferred.
    CalibrationSelector::Options rawFirst;
    rawFirst.preferRefined = false;
    Result<CalibrationSet> rawPair = CalibrationSelector::select(s, rawFirst, &warnings);
    REQUIRE(rawPair.ok());
    REQUIRE(rawPair.value().sourceSlave == "native_slave");
    REQUIRE(rawPair.value().sourceMaster == "native_master");
    REQUIRE_THAT(rawPair.value().slave.fx, WithinRel(1042.22f, 1e-5f));
    REQUIRE(warnings.empty());
}

TEST_CASE("CalibrationSelector maps a stitching distance to the nearest far preset", "[meta][sample]") {
    OSV_REQUIRE_SAMPLE();
    const OsvFile file = openSample();
    Result<MetadataTrack> loaded = MetadataTrack::load(file);
    REQUIRE(loaded.ok());
    const StreamMeta& s = loaded.value().stream();

    const auto& presets = CalibrationSelector::farPresets();
    REQUIRE(presets.size() == 6);
    REQUIRE(presets[0].distanceM == 0.7);
    REQUIRE(presets[0].slaveSlot == PanoDewarpParams::Far07Slave);
    REQUIRE(presets[5].distanceM == 1.6);
    REQUIRE(presets[5].masterSlot == PanoDewarpParams::Far16Master);

    struct Case {
        double distance;
        const char* slave;
        double expectedFx;
    };
    // 1.0 m is exactly between 0.9 and 1.1: the documented tie rule picks
    // the smaller distance (0.9 m).
    const Case cases[] = {
        {0.1, "far_07_slave", 1042.24},  {0.7, "far_07_slave", 1042.24}, {0.85, "far_09_slave", 1041.78},
        {1.0, "far_09_slave", 1041.78},  {1.05, "far_11_slave", 1041.31}, {1.2, "far_12_5_slave", 1040.97},
        {1.3, "far_12_5_slave", 1040.97}, {1.45, "far_14_slave", 1040.62}, {1.55, "far_16_slave", 1040.16},
        {50.0, "far_16_slave", 1040.16},
    };
    for (const Case& c : cases) {
        INFO("distance " << c.distance);
        std::vector<std::string> warnings;
        CalibrationSelector::Options opt;
        opt.stitchDistanceM = c.distance;
        Result<CalibrationSet> selected = CalibrationSelector::select(s, opt, &warnings);
        REQUIRE(selected.ok());
        REQUIRE(selected.value().sourceSlave == c.slave);
        REQUIRE(selected.value().sourceMaster == std::string(c.slave).replace(std::string(c.slave).size() - 5, 5, "master"));
        REQUIRE_THAT(static_cast<double>(selected.value().slave.fx), WithinRel(c.expectedFx, 1e-5));
        // Exact preset distances are silent, everything else notes the mapping.
        const bool exact = std::fabs(c.distance - 0.7) < 1e-9;
        REQUIRE(warnings.empty() == exact);
    }
    // nearestFarPreset directly.
    const auto nearest = CalibrationSelector::nearestFarPreset(s, 1.0);
    REQUIRE(nearest.has_value());
    REQUIRE(nearest->distanceM == 0.9);
    REQUIRE_FALSE(CalibrationSelector::nearestFarPreset(s, std::nan("")).has_value());

    // Lens-mode overrides on a native-only file fall back with warnings.
    std::vector<std::string> warnings;
    CalibrationSelector::Options guards;
    guards.lensModeOverride = ExtriLensMode::LensGuards;
    Result<CalibrationSet> guarded = CalibrationSelector::select(s, guards, &warnings);
    REQUIRE(guarded.ok());
    REQUIRE(guarded.value().sourceSlave == "native_refine_slave");
    REQUIRE(warnings.size() == 2);
    warnings.clear();
    CalibrationSelector::Options water;
    water.lensModeOverride = ExtriLensMode::Underwater;
    Result<CalibrationSet> wet = CalibrationSelector::select(s, water, &warnings);
    REQUIRE(wet.ok());
    REQUIRE(wet.value().sourceSlave == "native_refine_slave");
    REQUIRE(warnings.size() == 3);
}

// -----------------------------------------------------------------------------
//  LRF proxy
// -----------------------------------------------------------------------------

TEST_CASE("The LRF proxy is detected as side-by-side and carries the same calibration", "[meta][sample]") {
    OSV_REQUIRE_SAMPLE();
    if (!haveLrf()) {
        SKIP("LRF proxy not available: " << osvtest::sampleLrf().string());
    }
    const OsvFile osv = openSample();
    Result<OsvFile> lrfResult = OsvFile::open(osvtest::sampleLrf());
    REQUIRE(lrfResult.ok());
    const OsvFile& lrf = lrfResult.value();

    Result<MetadataTrack> osvMeta = MetadataTrack::load(osv);
    Result<MetadataTrack> lrfMeta = MetadataTrack::load(lrf);
    REQUIRE(osvMeta.ok());
    REQUIRE(lrfMeta.ok());
    REQUIRE(lrfMeta.value().hasCalibration());
    // Same clip, same serial number, same focal length.
    REQUIRE(lrfMeta.value().clip().header.serialNumber == osvMeta.value().clip().header.serialNumber);
    REQUIRE(lrfMeta.value().clip().digitalFocalLength == osvMeta.value().clip().digitalFocalLength);

    Result<FormatInfo> format = FormatDetector::detect(lrf, &lrfMeta.value());
    REQUIRE(format.ok());
    const FormatInfo& f = format.value();
    REQUIRE(f.mode == Mode::Lrf);
    REQUIRE(f.sideBySideProxy);
    REQUIRE(f.dualFisheye);
    REQUIRE(f.videoTrackIds[0] == f.videoTrackIds[1]);
    REQUIRE(f.videoTrackIds[0] == 1);
    REQUIRE(f.streamW == 2048);
    REQUIRE(f.streamH == 1024);
    REQUIRE(f.bitDepth == 8);
    REQUIRE(f.colorMode == ColorMode::DLogM);
    REQUIRE(f.colorModeFromMetadata);
    REQUIRE_THAT(f.fps, WithinAbs(29.97, 0.01));
    REQUIRE(f.cameraModel == "Osmo 360");

    Result<MetadataTrack::LensStreamOrder> order = lrfMeta.value().lensStreamOrder();
    REQUIRE(order.ok());
    REQUIRE(order.value().sideBySide);
    REQUIRE_FALSE(order.value().fromFallback);

    // The calibration pair is identical to the OSV's.
    Result<CalibrationSet> osvSet = CalibrationSelector::select(osvMeta.value().stream());
    Result<CalibrationSet> lrfSet = CalibrationSelector::select(lrfMeta.value().stream());
    REQUIRE(osvSet.ok());
    REQUIRE(lrfSet.ok());
    REQUIRE(lrfSet.value().sourceSlave == osvSet.value().sourceSlave);
    requireJsonMatches(toJson(lrfSet.value()), toJson(osvSet.value()), "lrfCalibration");
    // Every populated slot matches, not only the selected pair.
    requireJsonMatches(toJson(lrfMeta.value().stream().dewarp), toJson(osvMeta.value().stream().dewarp), "lrfDewarp");

    // The proxy has its own frame metadata (one sample per proxy frame).
    REQUIRE(lrfMeta.value().frameCount() == lrf.track(lrfMeta.value().trackId())->samples.count());
    REQUIRE(lrfMeta.value().frameCount() > 0);
    Result<FrameMeta> frame0 = lrfMeta.value().frame(0);
    REQUIRE(frame0.ok());
    REQUIRE(frame0.value().camera.attitude.present);
}
