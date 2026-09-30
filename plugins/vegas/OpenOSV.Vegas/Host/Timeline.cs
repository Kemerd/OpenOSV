// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Timeline.cs - selections, tracks, event time and pan/crop, through the VEGAS
// scripting API.

using System;
using System.Collections.Generic;
using OpenOSV.Vegas.Core;
using ScriptPortal.Vegas;

namespace OpenOSV.Vegas.Host
{
    /// <summary>Timeline helpers.  Every method is null-safe and never throws.</summary>
    internal static class Timeline
    {
        /// <summary>The selected video events, in track then time order.</summary>
        public static List<VideoEvent> SelectedVideoEvents(Project project)
        {
            var list = new List<VideoEvent>();
            if (project is null)
            {
                return list;
            }
            try
            {
                foreach (Track track in project.Tracks)
                {
                    if (!(track is VideoTrack))
                    {
                        continue;
                    }
                    foreach (TrackEvent evt in track.Events)
                    {
                        if (evt is VideoEvent ve && SafeSelected(ve))
                        {
                            list.Add(ve);
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                Log.Warn("timeline: reading the selection failed", ex);
            }
            return list;
        }

        /// <summary>The first selected track of a kind, or null.</summary>
        public static T FirstSelectedTrack<T>(Project project) where T : Track
        {
            if (project is null)
            {
                return null;
            }
            try
            {
                foreach (Track track in project.Tracks)
                {
                    if (track is T typed && track.Selected)
                    {
                        return typed;
                    }
                }
            }
            catch (Exception ex)
            {
                Log.Warn("timeline: reading the selected tracks failed", ex);
            }
            return null;
        }

        /// <summary>
        /// The time in an event's MEDIA at the playhead - where a keyframe on a
        /// generated media's parameters goes: playhead - event start + take
        /// offset, or the take offset (the event's first frame) when the
        /// playhead is outside the event.
        /// </summary>
        public static Timecode MediaTimeAtCursor(TrackEvent evt)
        {
            try
            {
                long offset = evt.ActiveTake?.Offset?.Nanos ?? 0;
                long start = evt.Start.Nanos;
                long end = start + evt.Length.Nanos;
                long cursor = VegasHost.Cursor.Nanos;
                long local = (cursor >= start && cursor < end) ? cursor - start : 0;
                return Timecode.FromNanos(Math.Max(0, local + offset));
            }
            catch (Exception)
            {
                return Timecode.FromNanos(0);
            }
        }

        /// <summary>
        /// The time in an event's own effect chain at the playhead (event FX
        /// keyframes count from the event's start), or zero outside the event.
        /// </summary>
        public static Timecode EventTimeAtCursor(TrackEvent evt)
        {
            try
            {
                long start = evt.Start.Nanos;
                long end = start + evt.Length.Nanos;
                long cursor = VegasHost.Cursor.Nanos;
                return Timecode.FromNanos((cursor >= start && cursor < end) ? cursor - start : 0);
            }
            catch (Exception)
            {
                return Timecode.FromNanos(0);
            }
        }

        /// <summary>The event's OpenOSV 360 Reframe effect, or null.</summary>
        public static Effect ReframeEffectOf(VideoEvent evt)
        {
            try
            {
                foreach (Effect fx in evt.Effects)
                {
                    if (VegasHost.IsPlugIn(fx, OfxIds.ReframePluginId))
                    {
                        return fx;
                    }
                }
            }
            catch (Exception)
            {
                // No effects to list.
            }
            return null;
        }

        /// <summary>
        /// Make an event's pan/crop show its whole source frame stretched over
        /// the whole output frame.
        /// </summary>
        /// <remarks>
        /// <para>
        /// VEGAS maps each event into the project frame through the pan/crop
        /// keyframes: a keyframe's Bounds is the rectangle of the SOURCE (in
        /// source pixels) that fills the output frame.  With "Maintain aspect
        /// ratio" on, VEGAS widens that rectangle to the output's shape, which
        /// letterboxes a 2:1 sphere inside a 16:9 frame - and a reframe filter
        /// that treats the whole image it is given as the sphere would then
        /// see black bars as part of the sphere.
        /// </para>
        /// <para>
        /// So: aspect ratio off, and the one keyframe's Bounds set to exactly
        /// the source frame, which stretches the sphere corner to corner (the
        /// same "stretch" docs/RESOLVE.md asks Resolve users for).  Stretching
        /// distorts nothing the filter cares about - it maps the sphere by
        /// position, not by pixel aspect.  An event whose pan/crop is animated
        /// (more than one keyframe) is somebody's deliberate move and is left
        /// alone; the caller reports it.
        /// </para>
        /// </remarks>
        /// <returns>True when the event now stretches its full source frame.</returns>
        public static bool StretchToFill(VideoEvent evt)
        {
            try
            {
                evt.MaintainAspectRatio = false;
            }
            catch (Exception ex)
            {
                Log.Warn("timeline: turning off Maintain aspect ratio failed", ex);
            }
            try
            {
                VideoStream stream = evt.ActiveTake?.MediaStream as VideoStream;
                if (stream is null)
                {
                    return false;
                }
                int w = stream.Width;
                int h = stream.Height;
                if (w <= 0 || h <= 0)
                {
                    return false;
                }
                VideoMotionKeyframes keys = evt.VideoMotion.Keyframes;
                if (keys.Count != 1)
                {
                    Log.Info("timeline: event '" + VegasHost.SafeString(() => evt.Name) + "' has an animated pan/crop; left as it is");
                    return false;
                }
                VideoMotionKeyframe k = keys[0];
                k.Bounds = new VideoMotionBounds(0f, 0f, w, 0f, w, h, 0f, h);
                k.Center = new VideoMotionVertex(w / 2f, h / 2f);
                k.Rotation = 0.0;
                return true;
            }
            catch (Exception ex)
            {
                Log.Warn("timeline: stretching the pan/crop failed", ex);
                return false;
            }
        }

        /// <summary>
        /// After a media's frame size changed, move an event's pan/crop to the
        /// new full frame - but only when it still showed the OLD full frame
        /// (the default VEGAS made), never a framing someone chose.
        /// </summary>
        public static bool RefitUntouchedPanCrop(VideoEvent evt, int oldWidth, int oldHeight, int newWidth, int newHeight)
        {
            try
            {
                VideoMotionKeyframes keys = evt.VideoMotion.Keyframes;
                if (keys.Count != 1)
                {
                    return false;
                }
                VideoMotionKeyframe k = keys[0];
                VideoMotionBounds b = k.Bounds;
                bool wasFullFrame = Near(b.TopLeft.X, 0) && Near(b.TopLeft.Y, 0) && Near(b.BottomRight.X, oldWidth) &&
                                    Near(b.BottomRight.Y, oldHeight) && Near(b.TopRight.X, oldWidth) && Near(b.BottomLeft.Y, oldHeight);
                if (!wasFullFrame)
                {
                    return false;
                }
                k.Bounds = new VideoMotionBounds(0f, 0f, newWidth, 0f, newWidth, newHeight, 0f, newHeight);
                k.Center = new VideoMotionVertex(newWidth / 2f, newHeight / 2f);
                return true;
            }
            catch (Exception ex)
            {
                Log.Debug("timeline: refitting a pan/crop failed: " + ex.Message);
                return false;
            }
        }

        private static bool Near(float a, float b) => Math.Abs(a - b) < 0.75f;

        private static bool SafeSelected(TrackEvent evt)
        {
            try
            {
                return evt.Selected;
            }
            catch (Exception)
            {
                return false;
            }
        }
    }
}
