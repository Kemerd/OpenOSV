// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Targets.cs - which cameras an action changes: the generators behind the
// selected OSV events, and the OpenOSV 360 Reframe effects on the rest.

using System;
using System.Collections.Generic;
using OpenOSV.Vegas.Core;
using OpenOSV.Vegas.Host;
using ScriptPortal.Vegas;

namespace OpenOSV.Vegas.Actions
{
    /// <summary>One camera to change: an OpenFX effect, the event it was reached from, and its frame.</summary>
    internal sealed class CameraTarget
    {
        /// <summary>The effect's OpenFX parameters.</summary>
        public OFXEffect Fx;

        /// <summary>The OpenOSV media when the camera is a generator's (null for a filter).</summary>
        public OsvMedia Media;

        /// <summary>The event it was selected through (for the key time).</summary>
        public VideoEvent Event;

        /// <summary>Where a keyframe goes when a control is animated.</summary>
        public Timecode KeyTime;

        /// <summary>The frame the camera renders (for the DJI numbers' shape).</summary>
        public int FrameWidth;

        /// <summary>Its height.</summary>
        public int FrameHeight;

        /// <summary>True when other events share this generator's media.</summary>
        public bool Shared;
    }

    /// <summary>Collects the cameras an action works on.</summary>
    internal static class Targets
    {
        /// <summary>
        /// The cameras of the selected events: each OSV media once (the first
        /// selected event gives the key time), and each event's own OpenOSV
        /// 360 Reframe effect.  With no selection, <paramref name="fallback"/>
        /// (the clip chosen in the panel) is the only target.
        /// </summary>
        public static List<CameraTarget> Cameras(Project project, OsvMedia fallback, bool includeFilters, out int osvEvents, out int otherEvents)
        {
            osvEvents = 0;
            otherEvents = 0;
            var list = new List<CameraTarget>();
            var seen = new HashSet<string>(StringComparer.Ordinal);
            VegasHost.FrameSize(project, out int projectW, out int projectH);
            List<VideoEvent> selected = Timeline.SelectedVideoEvents(project);
            Dictionary<string, int> counts = Timeline.EventCountsByMediaKey(project);

            foreach (VideoEvent evt in selected)
            {
                OsvMedia media = OsvMedia.ForEvent(evt);
                if (media is not null)
                {
                    ++osvEvents;
                    if (!seen.Add(media.Key))
                    {
                        continue;
                    }
                    list.Add(ForMedia(media, evt, counts));
                    continue;
                }
                ++otherEvents;
                if (!includeFilters)
                {
                    continue;
                }
                Effect filter = Timeline.ReframeEffectOf(evt);
                OFXEffect fx = Ofx.Of(filter);
                if (fx is not null)
                {
                    list.Add(new CameraTarget
                    {
                        Fx = fx,
                        Event = evt,
                        KeyTime = Timeline.EventTimeAtCursor(evt),
                        FrameWidth = projectW,
                        FrameHeight = projectH,
                    });
                }
            }

            // ---- nothing selected: the clip chosen in the panel ------------------------------
            if (selected.Count == 0 && fallback is not null)
            {
                list.Add(ForMedia(fallback, null, counts));
            }
            return list;
        }

        /// <summary>The OSV media of the selected events (each once), else the fallback.</summary>
        public static List<OsvMedia> Media(Project project, OsvMedia fallback)
        {
            var list = new List<OsvMedia>();
            var seen = new HashSet<string>(StringComparer.Ordinal);
            List<VideoEvent> selected = Timeline.SelectedVideoEvents(project);
            foreach (VideoEvent evt in selected)
            {
                OsvMedia media = OsvMedia.ForEvent(evt);
                if (media is not null && seen.Add(media.Key))
                {
                    list.Add(media);
                }
            }
            if (selected.Count == 0 && fallback is not null)
            {
                list.Add(fallback);
            }
            return list;
        }

        private static CameraTarget ForMedia(OsvMedia media, VideoEvent evt, Dictionary<string, int> eventCounts)
        {
            System.Drawing.Size size = media.FrameSize;
            return new CameraTarget
            {
                Fx = media.Fx,
                Media = media,
                Event = evt,
                KeyTime = evt is not null ? Timeline.MediaTimeAtCursor(evt) : null,
                FrameWidth = size.Width,
                FrameHeight = size.Height,
                Shared = eventCounts.TryGetValue(media.Key, out int events) && events > 1,
            };
        }
    }
}
