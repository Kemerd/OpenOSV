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

#include <filesystem>

namespace osv::premiere {

/// Record the Source Settings a Premiere-opened instance of `path` now has,
/// so the engine's own instance of the same file renders with them.  Cheap
/// (a map insert under a mutex), never throws.
void enginePublishPrefs(const std::filesystem::path& path, const PrefsBlob& prefs) noexcept;

/// Drop every engine clip and its GPU decoder.  Called from imShutdown, while
/// the CUDA driver and Premiere's context are still alive - never from
/// DllMain.  Safe to call when the engine was never used.
void engineShutdown() noexcept;

}  // namespace osv::premiere
