// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxSource.h - "OpenOSV Source": a DJI Osmo 360 .OSV clip as an OpenFX
// generator.
//
// ===========================================================================
//  Why a generator
// ===========================================================================
// DaVinci Resolve cannot open an .OSV at all - two HEVC fisheye streams and a
// protobuf metadata track in one container - and OpenFX has no importer API.
// What OpenFX does have is the generator: an effect that makes pictures out
// of nothing but its parameters.  So the clip becomes a parameter:
//
//   file path  --> the importer's own engine (ImporterInstance), compiled into
//                  this module unchanged: container, calibration, lens rig,
//                  stabilisation, seam search, parallax, sky seam fix, lens
//                  shading, sun ghost removal, colour - the Premiere stitch,
//                  bit for bit
//              --> the stitched sphere
//              --> either the sphere itself ("360 equirect"), rendered
//                  straight at the timeline's size, or a virtual camera into
//                  it ("Reframed view") with the OpenOSV 360 Reframe controls.
//
// Reframing INSIDE the generator matters in Resolve: the Edit page scales a
// clip to the timeline before any effect sees it, so a 2:1 sphere dropped on
// a 16:9 timeline reaches a separate reframe effect squeezed (or letterboxed)
// and resampled once already.  The generator hands the camera the native
// sphere instead.
//
// What an OpenFX generator cannot do: carry the clip's audio, or tell the
// host how long it is.  The Clip read-out shows the duration to trim to, and
// docs/RESOLVE.md shows how to bring the audio in.
#pragma once

#include "OfxHost.h"

namespace osv::ofx::source {

/// The plug-in identifier (stored in Resolve projects: NEVER change it).
inline constexpr const char* kPluginId = "org.openosv.OSVSource";

/// Version reported to the host (see OfxReframe.h on when MAJOR moves).
inline constexpr unsigned kVersionMajor = 1;
inline constexpr unsigned kVersionMinor = 0;

// ---- the generator's own parameters (permanent names) ------------------------
inline constexpr const char* kFile = "file";
inline constexpr const char* kChooseFile = "chooseFile";
inline constexpr const char* kClipInfo = "clipInfo";
inline constexpr const char* kOutput = "output";
inline constexpr const char* kStartFrame = "startFrame";

/// Output choice items, 0-based.
inline constexpr int kOutputReframed = 0;
inline constexpr int kOutputEquirect = 1;
inline constexpr const char* kOutputItems = "Reframed view|360 equirect";

/// The OfxPlugin::mainEntry of the generator.  Never throws.
OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                    OfxPropertySetHandle outArgs) noexcept;

/// Drop every clip the module still caches (the last kOfxActionUnload, before
/// the renderers and decoders are torn down).
void shutdown() noexcept;

/// The clip frame shown at host time `time`.
///
/// `rangeStart` is the first frame of the generator's output clip
/// (kOfxImageEffectPropFrameRange[0], 0 when the host does not say),
/// `hostFps` the timeline's frame rate, `clipFps` the .OSV's own, and
/// `startFrame` the Start Frame control.  Returns a negative number for a
/// time before the clip (the caller renders transparent black there).
///
/// THE MAPPING
///   local   = time - rangeStart      frames since the generator's first frame
///   seconds = local / hostFps
///   frame   = startFrame + floor(seconds * clipFps + 1e-4)
///
/// Frames are mapped through SECONDS, so a 59.94 fps clip on a 29.97 fps
/// timeline plays at its real speed (every other frame) instead of in slow
/// motion.  The tiny epsilon keeps an exact frame boundary (1001/60000 s
/// times 60000/1001) from rounding down to the previous frame.
///
/// One guard, because DaVinci Resolve does not document what time it gives a
/// generator: a range start the render time has not even reached (local
/// clearly negative) can only mean the host counts time from zero while
/// reporting the range in timeline frames, so the time is taken as it is.
/// A negative offset is never a real position inside a clip, so this cannot
/// misplace a frame the plain formula would have placed correctly.
[[nodiscard]] inline long long frameForTime(double time, double rangeStart, double hostFps, double clipFps,
                                            long long startFrame) noexcept {
    // Unusable rates: 1:1 frame mapping is the only safe reading.
    if (!(hostFps > 0.0) || !(hostFps < 1000.0)) {
        hostFps = (clipFps > 0.0 && clipFps < 1000.0) ? clipFps : 30.0;
    }
    if (!(clipFps > 0.0) || !(clipFps < 1000.0)) {
        clipFps = hostFps;
    }
    if (!(time == time) || !(rangeStart == rangeStart)) {  // NaN
        return -1;
    }
    double local = time - rangeStart;
    if (local < -0.5) {
        local = time;
    }
    const double seconds = local / hostFps;
    const double frame = seconds * clipFps + 1e-4;
    if (frame < 0.0) {
        return -1;
    }
    if (frame > 9.0e15) {
        return -1;  // absurd; never a frame of a real clip
    }
    const long long base = static_cast<long long>(frame);  // floor for non-negative
    if (startFrame < 0) {
        startFrame = 0;
    }
    return base + startFrame;
}

}  // namespace osv::ofx::source
