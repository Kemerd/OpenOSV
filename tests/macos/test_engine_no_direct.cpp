// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The importer's engine hooks on a build without the direct GPU path
// (plugins/importer/EngineNoDirect.cpp): the truthful answers the rest of
// the importer relies on.

#include <catch2/catch_test_macros.hpp>

#include "Engine.h"

#include <chrono>

using namespace osv::premiere;

TEST_CASE("without a direct path the engine hooks answer truthfully", "[macos][plugins][engine]") {
    // Publisher tokens: never 0 ("anonymous"), strictly increasing.
    const std::uint64_t a = engineNewPublisherToken();
    const std::uint64_t b = engineNewPublisherToken();
    CHECK(a != 0u);
    CHECK(b > a);

    const std::filesystem::path clip = "/Volumes/Card/DCIM/CAM_0001.OSV";
    SettingsPublisher publisher;
    publisher.token = b;
    enginePublishPrefs(clip, PrefsBlob::defaults(), publisher);

    // No direct frame is ever served, so the importer's own frame stays the
    // picture and its quality is never lowered for a direct path.
    engineNoteDirectFrame(clip);
    CHECK_FALSE(engineDirectPathActive(clip));
    CHECK_FALSE(engineDirectPathActive(clip, std::chrono::hours(24)));
    CHECK(engineDirectFrameCount(clip) == 0u);

    // Shutting down an engine that holds nothing is harmless, twice.
    engineShutdown();
    engineShutdown();
    SUCCEED();
}
