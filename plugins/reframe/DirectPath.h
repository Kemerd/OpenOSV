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
//     DirectLaunch.h);
//   * [WP-SETTINGS] the clip's Source Settings as the engine will render
//     them, and the rule (DirectPathSettings.h) that hands a clip to the
//     equirect route whenever the direct path could not reproduce what that
//     route would show - unknown settings, the D-Log M passthrough, or a
//     colour output other than the working space.
//
// Everything here is best effort by design: any missing piece is reported
// once in the log and the caller falls back to the equirect path, which is
// the behaviour that existed before this file.
#pragma once

#include "DirectLaunch.h"
#include "DirectPathSettings.h"
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
    /// [WP-SETTINGS] Optional: the cheap "which Source Settings would you
    /// render" question.  Without it the settings reported with each frame
    /// decide instead, after the decode rather than before it.
    OsvEngineQuerySettingsFn querySettings = nullptr;
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
    /// Frames of this instance whose clip time -> media time -> frame mapping
    /// renderDirect() has logged (the first few of every instance, so a field
    /// session can check trimmed / sped-up / reversed clips).  A diagnostic
    /// counter: read and written only under the mapping logger's own lock.
    mutable int mappingFramesLogged = 0;

    // ---- [WP-SETTINGS] how Premiere identifies the clip's media ------------
    // Evidence for the log, read in the same property pass as the path: does
    // Premiere's own identity of the media change when its Source Settings
    // do?  (That is what decides whether it re-renders the effect.)
    std::string mediaHash;      ///< GetNodeInfo's hash of the media node (a GUID string).
    std::string modState;       ///< "MediaNode::MediaModState".
    std::string clipId;         ///< "MediaNode::ClipID" (compare with the importer id the importer logs).
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
///
/// [WP-SETTINGS] Before decoding anything it asks the engine which Source
/// Settings it would render the clip with and applies decideSettings()
/// (DirectPathSettings.h); a clip the rule hands to the equirect route
/// returns false with a reason for which isPolicyFallback() is true - not a
/// failure, already logged once per change, and to be re-asked next frame.
[[nodiscard]] bool renderDirect(const DirectRequest& request, std::string& reason) noexcept;

}  // namespace osv::reframe::direct
