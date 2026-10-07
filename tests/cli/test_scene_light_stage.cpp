// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The importer's SceneLightStage (plugins/importer/SceneLightStage.h): how it
// recognises a request it is already working for, and what it lets into the
// process-wide answer cache.  Both cases use files no decoder can open, so
// the worker fails at once and nothing here opens NVDEC, CUDA or D3D11VA.
//
//   * A clip whose size or write time cannot be read must still be the SAME
//     request frame after frame: restarting the job on every frame meant a
//     playback of Interactive frames never let it finish.
//   * A clip whose sample frames would not decode must NOT be cached as its
//     answer: that would serve "no sky, day profile" to every later instance
//     of the clip for the rest of the session.

#include <catch2/catch_test_macros.hpp>

#include "SceneLightStage.h"

#include "osv/color/ColorParams.h"
#include "osv/core/Math.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace osv;
using osv::premiere::SceneLightRequest;
using osv::premiere::SceneLightStage;

namespace {

/// How long a test waits for a worker that fails at its first decode.
constexpr std::chrono::milliseconds kSettle{20000};

/// A request for `path`: one sample frame levelled with +Z up, the default
/// rig and blend, a linear decode.  The file is never decodable here.
[[nodiscard]] SceneLightRequest requestFor(const std::filesystem::path& path) {
    SceneLightRequest r;
    r.path = path;
    r.frames = {0u};
    r.upBody = {Vec3d{0.0, 0.0, 1.0}};
    r.linearColor = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f);
    return r;
}

/// A scratch file of `bytes` non-video bytes under the test output directory
/// (a real file, so its size and write time CAN be read).
[[nodiscard]] std::filesystem::path notAClip(const std::string& name, std::size_t bytes) {
    const std::filesystem::path dir = std::filesystem::path(OSV_TEST_OUTPUT_DIR) / "scene_light_stage";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path p = dir / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    for (std::size_t i = 0; i < bytes; ++i) {
        out.put(static_cast<char>((i * 131u + 7u) & 0xFFu));
    }
    return p;
}

}  // namespace

TEST_CASE("SceneLightStage: a clip it cannot inspect is still the same request", "[scenelight][stage]") {
    // No such file: absolute() works, file_size() does not, so the request
    // cannot be shared - but this instance must still know it again.
    const std::filesystem::path missing =
        std::filesystem::path(OSV_TEST_OUTPUT_DIR) / "scene_light_stage" / "no_such_clip.OSV";
    std::error_code ec;
    std::filesystem::remove(missing, ec);
    const SceneLightRequest r = requestFor(missing);

    SceneLightStage stage;
    stage.request(r, "no_such_clip.OSV");
    REQUIRE(stage.waitSettled(kSettle));
    const SceneLightStage::Snapshot first = stage.snapshot();
    REQUIRE(first.active);
    REQUIRE(first.settled);
    CHECK_FALSE(first.failure.empty());
    CHECK_FALSE(first.fromCache);
    CHECK((!first.cap || !first.cap->valid));

    // ---- the same request again: kept, not restarted ------------------------------
    // A restart would reset the state to "measuring" and run a new job; the
    // answer must instead stay exactly the one already published.
    for (int i = 0; i < 5; ++i) {
        stage.request(r, "no_such_clip.OSV");
        const SceneLightStage::Snapshot again = stage.snapshot();
        INFO("repeat " << i);
        CHECK(again.settled);
        CHECK(again.failure == first.failure);
        CHECK(again.millis == first.millis);
    }

    // ---- a different request (another frame) is a new one ---------------------------
    SceneLightRequest other = r;
    other.frames = {1u};
    stage.request(other, "no_such_clip.OSV");
    REQUIRE(stage.waitSettled(kSettle));
    CHECK(stage.snapshot().settled);

    // ---- stop() leaves the stage ready to take the request afresh ------------------------
    stage.stop();
    CHECK_FALSE(stage.snapshot().active);
    stage.request(r, "no_such_clip.OSV");
    REQUIRE(stage.waitSettled(kSettle));
    CHECK(stage.snapshot().settled);
}

TEST_CASE("SceneLightStage: frames that would not decode are not cached as the clip's answer",
          "[scenelight][stage]") {
    // A real, inspectable file (so the request IS shareable) that no decoder
    // opens: every sample frame fails to decode.
    const std::filesystem::path junk = notAClip("not_a_clip.OSV", 4096);
    REQUIRE(std::filesystem::exists(junk));
    const SceneLightRequest r = requestFor(junk);

    // ---- the first instance measures and fails ---------------------------------------
    SceneLightStage a;
    a.request(r, "not_a_clip.OSV");
    REQUIRE(a.waitSettled(kSettle));
    const SceneLightStage::Snapshot sa = a.snapshot();
    REQUIRE(sa.settled);
    CHECK_FALSE(sa.fromCache);
    CHECK((!sa.cap || !sa.cap->valid));
    CHECK(sa.failure.find("could not be decoded") != std::string::npos);

    // ---- a second instance of the same clip measures again --------------------------------
    // A cached failure would come back at once with fromCache set.
    SceneLightStage b;
    b.request(r, "not_a_clip.OSV");
    REQUIRE(b.waitSettled(kSettle));
    const SceneLightStage::Snapshot sb = b.snapshot();
    REQUIRE(sb.settled);
    CHECK_FALSE(sb.fromCache);
    CHECK(sb.failure.find("could not be decoded") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(junk, ec);
}
