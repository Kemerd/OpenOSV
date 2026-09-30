// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ProxyTiming.cs - keep a clip on the same moments when its generator swaps
// between the .OSV and its .LRF proxy.
//
// ===========================================================================
//  The problem
// ===========================================================================
// The camera writes the .LRF at a different rate from the .OSV (29.97 fps for
// a 59.94 fps recording) and does not start both on the same instant: on the
// sample clip the .OSV's first frame is 1.0677 s into the .LRF.  The importer
// handles this for Premiere by presenting the .LRF on the .OSV's timeline
// (ImporterInstance::adoptProxyTimelineLocked / sourceFrameFor): both files
// carry the camera's own microsecond timestamp per frame, on one clock.
//
// OpenOSV Source maps host time to a clip frame through SECONDS
// (OfxSource.h, frameForTime):
//
//     frame = startFrame + floor(secondsSinceGeneratorStart * clipFps)
//
// so the generator already plays either file at real speed.  What differs is
// where each file's frame 0 sits on the camera's clock.  Swapping the file
// therefore only has to move Start Frame to the frame of the NEW file that
// was recorded at the moment the OLD start frame shows:
//
//     moment  = ts0(old) + startFrame(old) / fps(old)
//     startFrame(new) = round((moment - ts0(new)) * fps(new))
//
// clamped into the new file.  Everything else - the media's length, the
// events, the audio - stays exactly where it was.  Rounding costs at most half
// a proxy frame (17 ms at 29.97 fps), the same the importer's nearest-frame
// rule costs; the extension remembers the original Start Frame, so going back
// to full quality restores it exactly rather than rounding twice.

using System;

namespace OpenOSV.Vegas.Core
{
    /// <summary>Start Frame conversion between an .OSV and its .LRF.</summary>
    public static class ProxyTiming
    {
        /// <summary>
        /// The Start Frame in <paramref name="to"/> that shows the moment
        /// <paramref name="fromStartFrame"/> shows in <paramref name="from"/>.
        /// When either file lacks a camera timestamp the two are taken to start
        /// together (how the camera writes them).  The result is clamped into
        /// [0, frames - 1] of the target (0 for an empty target).
        /// </summary>
        public static long MapStartFrame(long fromStartFrame, ProbeResult from, ProbeResult to)
        {
            if (from == null || to == null || !from.Fps.IsValid || !to.Fps.IsValid)
            {
                return Math.Max(0, fromStartFrame);
            }
            return MapStartFrame(fromStartFrame, from.Fps, from.FirstFrameTimestampUs, to.Fps, to.FirstFrameTimestampUs,
                                 to.FrameCount);
        }

        /// <summary>The same mapping from the raw numbers (see the other overload).</summary>
        public static long MapStartFrame(long fromStartFrame, Rational fromFps, long? fromTs0Us, Rational toFps, long? toTs0Us,
                                         long toFrameCount)
        {
            if (!fromFps.IsValid || !toFps.IsValid)
            {
                return Math.Max(0, fromStartFrame);
            }
            long start = Math.Max(0, fromStartFrame);

            // ---- the old start frame's moment, relative to the new file's frame 0 ----
            double offsetSeconds = 0.0;
            if (fromTs0Us.HasValue && toTs0Us.HasValue && fromTs0Us.Value > 0 && toTs0Us.Value > 0)
            {
                offsetSeconds = (fromTs0Us.Value - toTs0Us.Value) / 1e6;
            }
            double seconds = offsetSeconds + TimeMath.FramesToSeconds(start, fromFps);

            // ---- the nearest frame of the new file -----------------------------------------
            double frame = Math.Round(seconds * toFps.Value, MidpointRounding.AwayFromZero);
            if (double.IsNaN(frame) || frame < 0.0)
            {
                return 0;
            }
            if (toFrameCount > 0 && frame > toFrameCount - 1)
            {
                return toFrameCount - 1;
            }
            return (long)frame;
        }

        /// <summary>
        /// True when the proxy covers the original's moments at all (the
        /// importer's own test before it presents an .LRF as a proxy): the
        /// original must start before the proxy ends and end after it starts.
        /// </summary>
        public static bool Overlaps(ProbeResult original, ProbeResult proxy)
        {
            if (original == null || proxy == null || !original.IsUsable || !proxy.IsUsable)
            {
                return false;
            }
            double offset = 0.0;
            if (original.FirstFrameTimestampUs.HasValue && proxy.FirstFrameTimestampUs.HasValue)
            {
                offset = (original.FirstFrameTimestampUs.Value - proxy.FirstFrameTimestampUs.Value) / 1e6;
            }
            return offset < proxy.DurationSeconds && offset + original.DurationSeconds > 0.0;
        }
    }
}
