// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// EngineNoDirect.cpp - the importer side of Engine.h on a build without the
// direct GPU pipeline (macOS, where there is no CUDA).
//
// The direct path (docs/DIRECT_GPU.md) is the reframe effect decoding a
// clip's two fisheyes on NVDEC inside Premiere's CUDA context and rendering
// its view from them in one pass.  It is CUDA through and through, so a build
// without the CUDA toolkit has no direct path at all: Engine.cpp and its
// OsvEngine_* exports are not compiled, the effect renders every view from
// the importer's stitched equirect, and this file is what the rest of the
// importer calls instead.
//
// Every function below is the truthful answer for that build rather than a
// placeholder:
//
//   * publisher tokens are still handed out, monotonic and never 0, so the
//     callers' bookkeeping behaves exactly as on Windows;
//   * a publication has no reader - nothing renders from published settings
//     - so it is logged once and dropped;
//   * there is nothing to shut down;
//   * no direct frame is ever served, so the direct path is never active for
//     any file and its frame count is always 0, which keeps the importer's
//     own equirect at full quality (it is the only picture the effect has).

#include "Engine.h"

#include "PluginLog.h"

#include <atomic>

namespace osv::premiere {

namespace {

/// Next publisher token; starts at 1 because 0 means "anonymous".
std::atomic<std::uint64_t> g_nextPublisherToken{1};

}  // namespace

std::uint64_t engineNewPublisherToken() noexcept {
    return g_nextPublisherToken.fetch_add(1);
}

void enginePublishPrefs(const std::filesystem::path& path, const PrefsBlob& prefs,
                        const SettingsPublisher& publisher) noexcept {
    (void)prefs;
    (void)publisher;
    try {
        PluginLog::oncef("engine-no-direct", PluginLog::Level::Debug,
                         "direct: this build has no direct GPU path (no CUDA); '{}' and every other clip are "
                         "reframed from the importer's equirect",
                         path.filename().string());
    } catch (...) {
        // Formatting a log line is never worth failing the importer call.
    }
}

void engineShutdown() noexcept {
    // No engine clips, no decoders, no device memory: nothing to release.
}

void engineNoteDirectFrame(const std::filesystem::path& path) noexcept {
    // Only the direct path's own exports call this, and they do not exist in
    // this build.
    (void)path;
}

bool engineDirectPathActive(const std::filesystem::path& path, std::chrono::milliseconds window) noexcept {
    (void)path;
    (void)window;
    return false;
}

std::uint64_t engineDirectFrameCount(const std::filesystem::path& path) noexcept {
    (void)path;
    return 0;
}

}  // namespace osv::premiere
