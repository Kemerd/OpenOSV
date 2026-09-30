// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CameraActions.cs - DJI Studio's framing looks, keyframe easing presets and
// stabilisation switches, applied to the selected clips.

using System;
using System.Collections.Generic;
using OpenOSV.Vegas.Core;
using OpenOSV.Vegas.Host;
using ScriptPortal.Vegas;

namespace OpenOSV.Vegas.Actions
{
    /// <summary>The per-clip camera commands.</summary>
    internal static class CameraActions
    {
        /// <summary>
        /// Apply a framing look (Crystal Ball, Asteroid, Ultra Wide, Wide,
        /// Dewarp) to every selected clip's camera - the generator of an OSV
        /// event, the OpenOSV 360 Reframe effect of any other event.
        /// </summary>
        /// <remarks>
        /// Writes every value the effect's own Preset handler writes, in the
        /// order that ends in the same state whether or not VEGAS reports these
        /// scripted writes to the plug-in as user edits (DjiCamera.cs):
        /// the lens mirror, Classic FOV and Distortion, Tilt, Zoom, DJI FOV and
        /// Correction, Lens, and Preset last.  A keyframed control is keyed at
        /// the playhead instead of flattened.
        /// </remarks>
        public static ActionResult ApplyLook(string lookId, OsvMedia fallback)
        {
            const string title = "Framing look";
            FramingLook look = DjiCamera.LookById(lookId);
            if (look is null)
            {
                return ActionResult.Line(title, "Unknown look '" + lookId + "'.", StatusKind.Error);
            }
            Project project = VegasHost.Project;
            if (project is null)
            {
                return ActionResult.Blocking(title, Copy.NoProject);
            }
            List<CameraTarget> targets = Targets.Cameras(project, fallback, true, out int osvEvents, out int otherEvents);
            if (targets.Count == 0)
            {
                return ActionResult.Line(title, osvEvents + otherEvents == 0 ? Copy.SelectEventsFirst
                    : "No camera to frame: the selected events are neither OSV clips nor carry OpenOSV 360 Reframe.", StatusKind.Warning);
            }

            int done = 0;
            bool shared = false;
            using (new UndoBlock(project, "OpenOSV: " + look.Label))
            {
                foreach (CameraTarget t in targets)
                {
                    try
                    {
                        int resolution = Ofx.GetChoice(t.Fx, CameraParams.OutputResolution, 0);
                        double aspect = DjiCamera.FramingAspect(resolution, t.FrameWidth, t.FrameHeight);
                        FramingWrites w = DjiCamera.WritesFor(look, aspect);
                        Timecode key = t.KeyTime;
                        Ofx.SetBool(t.Fx, CameraParams.LensMirror, w.LensMirror);
                        Ofx.SetDouble(t.Fx, CameraParams.Fov, w.Fov, key);
                        Ofx.SetDouble(t.Fx, CameraParams.Distortion, w.Distortion, key);
                        Ofx.SetDouble(t.Fx, CameraParams.Tilt, w.Tilt, key);
                        Ofx.SetDouble(t.Fx, CameraParams.Zoom, w.Zoom, key);
                        Ofx.SetDouble(t.Fx, CameraParams.DjiFov, w.DjiFov, key);
                        Ofx.SetDouble(t.Fx, CameraParams.Correction, w.Correction, key);
                        Ofx.SetChoice(t.Fx, CameraParams.Lens, w.LensIndex0, Choices.Lens, key);
                        if (Ofx.SetChoice(t.Fx, CameraParams.Preset, w.PresetIndex0, Choices.Preset, key))
                        {
                            ++done;
                            shared |= t.Shared;
                        }
                    }
                    catch (Exception ex)
                    {
                        Log.Error("look: applying " + look.Label + " failed", ex);
                    }
                }
            }
            Session.Settings.LastLook = look.Id;
            Session.SaveSettings();
            return ActionResult.Line(title, Copy.LookApplied(look.Label, done, shared), done > 0 ? StatusKind.Success : StatusKind.Error);
        }

        /// <summary>Set the Keyframe Easing preset of every selected clip's camera.</summary>
        public static ActionResult ApplyEasing(string easingId, OsvMedia fallback)
        {
            const string title = "Keyframe easing";
            EasingPreset easing = DjiCamera.EasingById(easingId);
            if (easing is null)
            {
                return ActionResult.Line(title, "Unknown easing '" + easingId + "'.", StatusKind.Error);
            }
            Project project = VegasHost.Project;
            if (project is null)
            {
                return ActionResult.Blocking(title, Copy.NoProject);
            }
            List<CameraTarget> targets = Targets.Cameras(project, fallback, true, out int osvEvents, out int otherEvents);
            if (targets.Count == 0)
            {
                return ActionResult.Line(title, osvEvents + otherEvents == 0 ? Copy.SelectEventsFirst
                    : "No camera to ease: the selected events are neither OSV clips nor carry OpenOSV 360 Reframe.", StatusKind.Warning);
            }
            int done = 0;
            using (new UndoBlock(project, "OpenOSV: keyframe easing"))
            {
                foreach (CameraTarget t in targets)
                {
                    // The easing popup is a clip-wide choice, never keyed.
                    if (Ofx.SetChoice(t.Fx, CameraParams.KeyframeEasing, easing.Index0, Choices.KeyframeEasing))
                    {
                        ++done;
                    }
                }
            }
            Session.Settings.LastEasing = easing.Id;
            Session.SaveSettings();
            return ActionResult.Line(title, Copy.EasingApplied(easing.Label, done), done > 0 ? StatusKind.Success : StatusKind.Error);
        }

        /// <summary>
        /// DJI Studio's RockSteady / Horizon Leveling switches on the selected
        /// OSV clips (stabilisation lives on the generator, not on a filter).
        /// </summary>
        public static ActionResult ApplyStabilisation(bool rockSteady, bool horizonLeveling, OsvMedia fallback)
        {
            const string title = "Stabilisation";
            Project project = VegasHost.Project;
            if (project is null)
            {
                return ActionResult.Blocking(title, Copy.NoProject);
            }
            List<OsvMedia> media = Targets.Media(project, fallback);
            if (media.Count == 0)
            {
                return ActionResult.Line(title, Timeline.SelectedVideoEvents(project).Count == 0 ? Copy.SelectEventsFirst : Copy.NoOsvInSelection,
                                         StatusKind.Warning);
            }
            int index = Stabilisation.IndexFor(rockSteady, horizonLeveling);
            int done = 0, replacedFull = 0;
            using (new UndoBlock(project, "OpenOSV: stabilisation"))
            {
                foreach (OsvMedia m in media)
                {
                    bool wasFull = m.Stabilization == Choices.StabFull;
                    if (Ofx.SetChoice(m.Fx, StitchParams.Stabilization, index, Choices.Stabilization))
                    {
                        ++done;
                        if (wasFull && index != Choices.StabFull)
                        {
                            ++replacedFull;
                        }
                    }
                }
            }
            Session.Settings.RockSteady = rockSteady;
            Session.Settings.HorizonLeveling = horizonLeveling;
            Session.SaveSettings();
            Notifier.RaiseClipsChanged();
            return ActionResult.Line(title, Copy.StabilisationApplied(Stabilisation.Caption(index), done, replacedFull),
                                     done > 0 ? StatusKind.Success : StatusKind.Error);
        }
    }
}
