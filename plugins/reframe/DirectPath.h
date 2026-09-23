// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DirectPath.h - the effect's side of the direct GPU pipeline.
//
// Premiere hands the effect the importer's stitched equirect and the effect
// samples its view out of that.  The direct path instead renders the view
// straight from the two FISHEYE frames: sharper (one resampling, not two) and
// independent of how large an equirect the importer was asked for.  The
// pieces it joins (docs/DIRECT_GPU.md):
//
//   * which file and which media time this effect instance is rendering -
//     read from Premiere's segment graph (effect node -> owning clip node ->
//     its media node's "MediaNode::MediaInstanceString", and the clip node's
//     TransformNodeTime);
//   * the sequence's working colour space, because a render from the
//     fisheyes bypasses Premiere's source -> working conversion and must
//     produce the working space itself;
//   * the importer's engine (OsvEngineAbi.h), which decodes the two lenses
//     on NVDEC into Premiere's CUDA context and returns them with the clip's
//     stitch state;
//   * the fused kernel and its parameter builder (DirectRender.h,
//     DirectLaunch.h).
//
// Everything here is best effort by design: any missing piece is reported
// once in the log and the caller falls back to the equirect path, which is
// the behaviour that existed before this file.
#pragma once

#include "DirectLaunch.h"
#include "DirectRender.h"
#include "OsvEngineAbi.h"
#include "ReframeCpu.h"

#include "PrSDKSequenceInfoSuite.h"
#include "PrSDKVideoSegmentSuite.h"

#include <cuda.h>

#include <string>

struct SPBasicSuite;

namespace osv::reframe::direct {

/// The importer's engine entry points, resolved once per process.
struct EngineApi {
    OsvEngineAbiVersionFn version = nullptr;
    OsvEngineAcquireFrameFn acquire = nullptr;
    OsvEngineReleaseFrameFn release = nullptr;
    [[nodiscard]] bool ok() const noexcept { return version && acquire && release; }
};

/// The engine, or an empty EngineApi (with the reason logged once) when the
/// importer module is not loaded, lacks the exports, speaks another ABI
/// version, or the OSV_DISABLE_DIRECT environment variable is set to 1 (the
/// kill switch for A/B comparisons and for ruling the path out in the field).
[[nodiscard]] const EngineApi& engine() noexcept;

/// Where this effect instance's pixels come from.
struct SourceBinding {
    std::wstring path;          ///< The media file (UTF-16).
    csSDK_int32 ownerNode = 0;  ///< The owning clip node, ACQUIRED; release with releaseSource().
    bool ok = false;
    std::string reason;         ///< Why not, when !ok.
};

/// Walk effect node -> owner clip node -> input media node and read the
/// media file's path.  The owner node stays acquired (it is what maps clip
/// time to media time on every frame) and must be released with
/// releaseSource().  `segmentVersion` guards the v6+ entry points.
[[nodiscard]] SourceBinding resolveSource(const PrSDKVideoSegmentSuite* segment, int segmentVersion,
                                          SPBasicSuite* basic, csSDK_int32 effectNode) noexcept;

/// Release what resolveSource() acquired.  Idempotent.
void releaseSource(const PrSDKVideoSegmentSuite* segment, SourceBinding& binding) noexcept;

/// The OSV_TRANSFER_* id matching the sequence's working colour space, or -1
/// when it is one the direct path cannot produce (then the caller falls
/// back, so Premiere's own conversion of the importer's frame stays in
/// charge).  `reason` receives a one-line description either way.
[[nodiscard]] int workingTransfer(const PrSDKSequenceInfoSuite* sequence, int sequenceVersion, SPBasicSuite* basic,
                                  PrTimelineID timeline, std::string& reason) noexcept;

/// One frame through the direct path.
struct DirectRequest {
    const SourceBinding* source = nullptr;
    const PrSDKVideoSegmentSuite* segment = nullptr;
    PrTime clipTime = 0;
    int transfer = -1;                 ///< OSV_TRANSFER_* (workingTransfer()).
    CUcontext context = nullptr;       ///< Premiere's context, current on this thread.
    CUfunction kernel = nullptr;       ///< The fused kernel in that context.
    Settings settings;                 ///< The effect's controls at this time.
    DirectOutput output;               ///< Premiere's output frame.
    SizePx sequenceSize;               ///< For Match Sequence.
};

/// Render one frame straight from the fisheyes into `request.output`, and
/// wait for it (the same completion contract as the equirect kernel).
/// Returns true on success; false with `reason` filled on any failure, in
/// which case the output has not been written and the caller falls back.
[[nodiscard]] bool renderDirect(const DirectRequest& request, std::string& reason) noexcept;

}  // namespace osv::reframe::direct
