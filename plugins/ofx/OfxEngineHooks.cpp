// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxEngineHooks.cpp - the two Source Settings publication hooks the clip
// engine (plugins/importer/ImporterInstance.cpp) links against, for a module
// that has no Premiere direct path.
//
// In Premiere, plugins/importer/Engine.cpp serves Open 360 Reframe's direct
// GPU path: every importer instance Premiere opens publishes its Source
// Settings there, so the effect can render the same clip from the fisheyes
// with them (Engine.h, [WP-SETTINGS]).  That engine - its registry, its
// NVDEC decoders in Premiere's CUDA context and its exported OsvEngine_* C
// ABI - has no consumer in an OpenFX host, so it is not compiled into
// OpenOSV.ofx.  What IS compiled in is ImporterInstance, which calls these
// two functions from publishSettingsLocked() - and every clip this module
// opens is marked setEngineOwned(true), which makes that function return
// before it reaches either of them (OfxSource.cpp, acquireClip()).
//
// They are therefore defined here with the behaviour the declaration in
// Engine.h promises for a registry nobody reads: a unique, increasing token,
// and a publication that is accepted and has no effect.

#include "Engine.h"

#include <atomic>

namespace osv::premiere {

namespace {
/// Tokens start at 1: 0 means "anonymous" in SettingsPublisher.
std::atomic<std::uint64_t> g_nextToken{1};
}  // namespace

std::uint64_t engineNewPublisherToken() noexcept { return g_nextToken.fetch_add(1); }

void enginePublishPrefs(const std::filesystem::path& /*path*/, const PrefsBlob& /*prefs*/,
                        const SettingsPublisher& /*publisher*/) noexcept {
    // No direct path to publish to in an OpenFX host (see the file comment).
}

}  // namespace osv::premiere
