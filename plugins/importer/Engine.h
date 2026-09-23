// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Engine.h - the importer side of the direct GPU pipeline.
//
// The importer module exports a small C ABI (plugins/common/OsvEngineAbi.h)
// that the reframe effect calls to get a clip's two fisheye frames decoded
// on NVDEC into Premiere's own CUDA context, together with the clip's stitch
// state.  This header is the importer-internal part of it: the registry the
// exports keep, and the two hooks the rest of the importer calls into.
//
// WHY THE ENGINE KEEPS ITS OWN INSTANCES.  The ImporterInstance Premiere
// opened for a clip lives and dies by imOpenFile8 / imQuietFile /
// imCloseFile, on Premiere's schedule, and the effect can render a clip whose
// importer instance is quiet at that moment.  So the engine opens its own
// ImporterInstance per file (metadata parse, rig, analysis caches, NVDEC
// decoder) and keeps it for the session; the only thing it takes from
// Premiere's instances is the Source Settings the user chose for the clip,
// which they publish here whenever they change.
#pragma once

#include "PrefsBlob.h"

#include <cstdint>
#include <filesystem>

namespace osv::premiere {

// ---- [WP-SETTINGS] Source Settings publication ------------------------------

/// Who is publishing a clip's Source Settings.
///
/// Premiere keeps several importer instances of one file alive at once - a
/// Source Settings change opens a NEW one with the new blob while the old
/// one lingers - and an older instance can still be handed its old blob
/// afterwards.  Were the last publication simply to win, that stale blob
/// would silently undo the user's change in the direct path.  So every
/// publication carries the publishing instance's age, and an older instance
/// never overrides what a newer one published.
struct SettingsPublisher {
    /// From engineNewPublisherToken(), taken at the instance's first
    /// publication (right after imOpenFile8, at imGetInfo8): larger means
    /// opened later.  0 is "anonymous" and always the oldest.
    std::uint64_t token = 0;
    /// True when the blob came from the host.  False when the host handed
    /// the instance no blob at all and the instance's defaults are in force:
    /// that fills a blank (nothing published for the file yet) but never
    /// overrides settings some instance was actually given.
    bool fromHost = true;
    /// The host's importer id for the instance - log evidence only, to be
    /// matched against the media node's ClipID the effect logs.
    std::uint32_t importerId = 0;
};

/// A fresh publisher token (monotonic, process-wide, never 0).
[[nodiscard]] std::uint64_t engineNewPublisherToken() noexcept;

/// Record the Source Settings a Premiere-opened instance of `path` now has,
/// so the engine's own instance of the same file renders with them.  The
/// file is identified by its volume serial and file index, not by how the
/// path is spelled.  Cheap (a cached identity lookup and a map update under
/// a mutex), never throws.
void enginePublishPrefs(const std::filesystem::path& path, const PrefsBlob& prefs,
                        const SettingsPublisher& publisher) noexcept;

// ---- [/WP-SETTINGS] ---------------------------------------------------------

/// Drop every engine clip and its GPU decoder.  Called from imShutdown, while
/// the CUDA driver and Premiere's context are still alive - never from
/// DllMain.  Safe to call when the engine was never used.
void engineShutdown() noexcept;

}  // namespace osv::premiere
